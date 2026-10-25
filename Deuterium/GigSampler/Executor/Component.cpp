#include "Component.hpp"

#include <Execution/DocumentPlugin.hpp>

#include <score/tools/Bind.hpp>

#include <core/document/Document.hpp>

#include <QPointer>
#include <QTimer>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/detail/ssize.hpp>

#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>
#include <Deuterium/GigSampler/SamplerEngine.hpp>
#include <libremidi/detail/conversion.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <limits>

namespace Deuterium::Gig
{
namespace Executor
{

struct gig_voice
{
  const GigRegion* region{};
  AmpEnv amp_adsr;

  Biquad filter;
  LofiState lofi;
  PlayHead head;
  ResolvedLoop loop;
  Lfo lfo;
  Lfo vibLfo;          // file-specified vibrato (SF2 vibLfo generators)
  Lfo modLfo;          // file-specified modLfo (tremolo / wah / pitch)
  PitchEnv pitchEnv;
  BlockEnv filterEnv;
  BlockEnv fileModEnv; // file-specified modulation envelope (SF2 EG2)
  GlideState glide;

  int note{-1};
  int velocity{};
  int64_t startOrder{};
  bool playing{};
  bool released{};
  bool choked{};
  double chokeSeconds{0.3}; // fade-out once choked
  int channel{};            // MIDI channel of the key, 0-15
  int line{-1};             // mono-style line it sounds on, -1 = none
  int64_t startClock{};     // m_clock at the start, or the last legato move
  bool oneShotEff{};
  //! Started with note-offs ignored: stays so when the mode changes, and
  //! notes started otherwise still hear their note-off.
  bool ignoresNoteOff{};
  bool filterOn{};
  int filterTypeEff{};
  double cutoffBaseHz{20000.};
  double q{1.};
  double filterGain{1.}; // passband compensation for file-driven resonance
  double chokeGain{1.};
  double gain{1.};       // velocity * zone crossfade * region attenuation
  double randomSemis{};
  double velFcCents{};   // default velocity->cutoff modulator, fixed at note-on
  // gig: cents spanned by the note's cutoff (in 0..127 units) that EG2 and
  // LFO2 scale down from; 0 = no gig cutoff modulation
  double gigFcSpanCents{};
};

class gigsampler_node final : public ossia::graph_node
{
public:
  static constexpr int max_voices = 64;
  static constexpr int max_regions = 4096;

  gigsampler_node(ossia::execution_state& st)
      : m_st{st}
  {
    this->m_inlets.push_back(midi_in = new ossia::midi_inlet);
    // One value inlet per control, in SamplerControl order right after the
    // MIDI inlet: score binds the process inlets to these by index, which is
    // what makes the controls modulatable from the execution graph (LFOs,
    // automations, cables) and not only from the UI.
    for(int i = 0; i < ControlCount; i++)
    {
      auto inlet = new ossia::value_inlet;
      control_ins[i] = inlet;
      this->m_inlets.push_back(inlet);
    }
    this->m_outlets.push_back(audio_out = new ossia::audio_outlet);
    m_voices.resize(max_voices);
    // MIDI of a tick is gathered here on the audio thread: no reallocation
    // for any reasonable amount of it
    m_midiScratch.reserve(1024);
    // Controller defaults (General MIDI) for the crossfades reading them
    m_cc[7] = 100;
    m_cc[10] = 64;
    m_cc[11] = 127;
  }

  void reload(std::shared_ptr<GigFileInfo>&& info, double rate)
  {
    using namespace std;
    // Swap so that the previous file's sample data lands back in the caller's
    // argument: when called through in_exec, the executed command is
    // garbage-collected on the UI thread, keeping the (potentially huge)
    // deallocation off the audio thread.
    swap(info, m_gigInfo);
    m_sampleRate = rate;

    for(auto& v : m_voices)
      stop_voice(v);
    clear_keys();
    for(auto& l : m_lines)
      l.hasPitch = false;
    std::fill(std::begin(m_polyHasLast), std::end(m_polyHasLast), false);
  }

  void set_control(int control, const ossia::value& v)
  {
    if(control == EnvFromFile)
    {
      m_envFromFile = ossia::convert<bool>(v);
      resolve_time(Attack);
      resolve_time(Decay);
      resolve_time(Release);
      resolve_sustain();
      m_feedback[control][0] = m_envFromFile ? 1.f : 0.f;
      m_feedback[control][1] = 0.f;
      return;
    }
    if(control == Sustain)
    {
      // Negative values are the historical "use the file's envelope" sentinel
      const float f = ossia::convert<float>(v);
      m_sustainRaw = f;
      m_sustainIsSentinel = f < 0.f;
      resolve_sustain();
      m_feedback[control][0] = f;
      m_feedback[control][1] = 0.f;
      return;
    }
    if(isTimeControl(control))
    {
      if(auto vec = v.target<ossia::vec2f>())
      {
        auto& raw = m_timeRaw[control];
        raw.x = (*vec)[0];
        raw.sync = (*vec)[1] != 0.f; // widget convention: y == 0 is free-running
        raw.is_vec = true;
        resolve_time(control);
        m_feedback[control][0] = raw.x;
        m_feedback[control][1] = raw.sync ? 1.f : 0.f;
        return;
      }
      // Plain float (legacy documents, values mapped through the graph):
      // historical unit — seconds, except LfoRate where it is Hz.
      m_timeRaw[control].is_vec = false;
    }
    const int previousChannel = m_params.midiChannel;
    applySamplerControl(m_params, control, v);
    // Another channel filter could drop the note-offs of what plays now
    if(m_params.midiChannel != previousChannel && m_params.midiChannel != 0)
    {
      all_notes_off();
      clear_keys();
    }
    m_feedback[control][0] = ossia::convert<float>(v);
    m_feedback[control][1] = 0.f;
  }

  // Turns the stored {x, sync} of a time control into the engine's unit
  // (seconds, Hz for the LFO) at the current tempo.
  void resolve_time(int control) noexcept
  {
    const auto& raw = m_timeRaw[control];
    if(!raw.is_vec)
      return;
    const float secs = raw.sync ? syncTimeToSeconds(raw.x, m_tempo) : raw.x;
    switch(control)
    {
      case Attack:
        m_params.attack = m_envFromFile ? -1.f : secs;
        break;
      case Decay:
        m_params.decay = m_envFromFile ? -1.f : secs;
        break;
      case Release:
        m_params.release = m_envFromFile ? -1.f : secs;
        break;
      case FilterEnvAttack:
        m_params.filterEnvAttack = secs;
        break;
      case FilterEnvDecay:
        m_params.filterEnvDecay = secs;
        break;
      case FilterEnvRelease:
        m_params.filterEnvRelease = secs;
        break;
      case Glide:
        m_params.glide = secs;
        break;
      case MonoFade:
        m_params.monoFade = std::clamp(secs, 0.f, 10.f);
        break;
      case ChordWindow:
        m_params.chordWindow = std::clamp(secs, 0.f, 1.f);
        break;
      case LfoDelay:
        m_params.lfoDelay = secs;
        break;
      case PitchEnvDecay:
        m_params.pitchEnvDecay = secs;
        break;
      case LfoRate:
        m_params.lfoRate = 1.f / std::clamp(secs, 1e-3f, 1e3f);
        break;
    }
  }

  [[nodiscard]] std::string label() const noexcept override { return "gigsampler"; }

  void all_notes_off() noexcept override
  {
    for(auto& v : m_voices)
    {
      if(v.playing && !v.released)
      {
        v.amp_adsr.startRelease();
        v.filterEnv.release();
        v.fileModEnv.release();
        v.released = true;
      }
    }
  }

  void run(const ossia::token_request& tk, ossia::exec_state_facade estate) noexcept override
  {
    // Tempo-synced time controls follow the transport's tempo.
    if(tk.tempo > 0 && tk.tempo != m_tempo)
    {
      m_tempo = tk.tempo;
      for(int i = 0; i < ControlCount; i++)
        if(isTimeControl(i))
          resolve_time(i);
    }

    // Values arriving through the graph (cables, LFOs, automation) land in
    // the control ports; apply them before rendering. UI edits reach
    // m_params directly through set_control().
    for(int i = 0; i < ControlCount; i++)
      for(const ossia::timed_value& v : control_ins[i]->data.get_data())
        set_control(i, v.value);

    if(!m_gigInfo || m_gigInfo->selectedInstrument < 0
       || m_gigInfo->selectedInstrument >= std::ssize(m_gigInfo->instruments))
    {
      // No playable instrument: nothing may keep sounding or hold a slot
      for(auto& v : m_voices)
        if(v.playing)
          stop_voice(v);
      clear_keys();
      return;
    }

    // One graph cycle can invoke run() once per token request: grow the
    // buffer up to this token's window and never clear what previous tokens
    // already rendered.
    const auto timings = estate.timings(tk);
    const std::size_t needed = timings.start_sample + timings.length;

    this->audio_out->data.set_channels(2);
    for(int i = 0; i < 2; i++)
    {
      auto& channel = this->audio_out->data.get()[i];
      if(channel.size() < needed)
        channel.resize(needed, 0.);
    }

    auto& instr = m_gigInfo->instruments[m_gigInfo->selectedInstrument];

    // Notes triggered from the panel's keyboard / pads widget (queued from
    // the UI through in_exec, so this vector is only touched on this thread)
    for(const auto& n : m_uiNotes)
    {
      if(n.on && n.velocity > 0)
        note_on(instr, 0, n.note, n.velocity);
      else
        note_off(instr, 0, n.note);
    }
    m_uiNotes.clear();

    // Tick boundary: the inlet message list is cleared once per graph tick
    // while run() executes once per token request. Track the part of the
    // buffer already consumed so that late-stamped messages (transport
    // discontinuities stamp note-offs at 0, other tokens' windows) are
    // handled exactly once instead of silently dropped - every dropped
    // note-off is a stuck note.
    if(m_st.samples_since_start != m_lastTick)
    {
      m_lastTick = m_st.samples_since_start;
      m_midiConsumedUntil = 0;
    }
    const int64_t windowEnd = timings.start_sample + timings.length;

    m_midiScratch.clear();
    for(auto& mess : this->midi_in->data.messages)
    {
      const auto ts = (int64_t)mess.timestamp;
      if(ts < m_midiConsumedUntil)
        continue; // already handled by an earlier token this tick
      if(timings.length > 0 && ts >= windowEnd)
        continue; // left for a later token this tick

      uint8_t data[4]{};
      int n = cmidi2_convert_single_ump_to_midi1((uint8_t*)data, 3, mess.data);
      // Three-byte channel messages, and channel pressure (two bytes)
      if(n != 3 && !(n == 2 && (data[0] & 0xF0) == 0xD0))
        continue;
      m_midiScratch.push_back({ts, {data[0], data[1], n == 3 ? data[2] : uint8_t(0)}});
    }
    m_midiConsumedUntil = std::max(m_midiConsumedUntil, windowEnd);
    // Sources append their messages unmerged: order by time so that a
    // note-off cannot overtake the note-on it belongs to. Stable insertion
    // sort: no temporary buffer on the audio thread, and linear on the
    // (nearly always) already ordered input.
    for(std::size_t i = 1; i < m_midiScratch.size(); i++)
      for(std::size_t j = i; j > 0 && m_midiScratch[j].first < m_midiScratch[j - 1].first;
          j--)
        std::swap(m_midiScratch[j], m_midiScratch[j - 1]);

    // In a zero-length window (paused / speed 0) only releases go through:
    // a voice started now would neither be heard nor ever advanced
    const bool paused = timings.length == 0;
    for(const auto& [ts, data] : m_midiScratch)
    {
      if(!acceptsChannel(m_params, data[0]))
        continue;
      libremidi::message m{{data[0], data[1], data[2]}};
      const int note = data[1];
      const int velocity = data[2];
      const int channel = data[0] & 0x0F;

      switch(m.get_message_type())
      {
        case libremidi::message_type::NOTE_ON: {
          if(velocity == 0)
            goto note_off;
          if(!paused)
            note_on(instr, channel, note, velocity);
          break;
        }
        note_off:
        case libremidi::message_type::NOTE_OFF: {
          note_off(instr, channel, note);
          break;
        }
        case libremidi::message_type::PITCH_BEND: {
          if(!paused)
          {
            const int v14 = data[1] | (data[2] << 7);
            m_bend = (v14 - 8192) / 8192.0;
            m_bendCh[channel] = m_bend;
          }
          break;
        }
        case libremidi::message_type::CONTROL_CHANGE: {
          switch(data[1])
          {
            case 120: // All Sound Off
              for(auto& v : m_voices)
                stop_voice(v);
              clear_keys();
              break;
            case 121: // Reset All Controllers (RP-015: the channel's bend,
                      // pressure, modulation, expression and pedals; not
                      // volume, pan nor the effect depths)
              m_bend = 0.;
              m_bendCh[channel] = 0.;
              m_cc[1] = 0;
              m_cc[11] = 127;
              for(int c = 64; c <= 67; c++)
                m_cc[c] = 0;
              m_cc[128] = 0;
              break;
            case 123: // All Notes Off
              all_notes_off();
              clear_keys();
              break;
            default:
              // Controller-driven layer crossfades read them
              m_cc[data[1] & 127] = data[2] & 127;
              break;
          }
          break;
        }
        case libremidi::message_type::AFTERTOUCH:
          // Channel pressure, for the layer crossfades it drives
          m_cc[128] = data[1] & 127;
          break;
        default:
          break;
      }
    }

    // Poly modes: a gathered chord is paired once its window has closed
    if(m_pendingCount > 0 && !paused
       && m_clock - m_pendingSince >= (int64_t)(m_params.chordWindow * m_sampleRate))
      flush_pending(instr);

    // Block-accurate clock for note lengths (release-trigger attenuation)
    m_clock += timings.length;

    double* out_l = this->audio_out->data.channel(0).data() + timings.start_sample;
    double* out_r = this->audio_out->data.channel(1).data() + timings.start_sample;

    const auto& p = m_params;
    const int64_t xfadeFrames = (int64_t)(p.loopXfade * m_sampleRate);

    for(auto& voice : m_voices)
    {
      if(!voice.playing || !voice.region)
        continue;

      auto& region = *voice.region;
      if(!region.sample.data)
      {
        stop_voice(voice);
        continue;
      }
      auto& sampleData = *region.sample.data;
      const int channels = sampleData.size();
      if(channels == 0)
      {
        stop_voice(voice);
        continue;
      }

      // SF2 endAddrsOffset trims trailing frames off the playable range
      int64_t totalFrames = std::ssize(sampleData[0]);
      if(region.sampleEndOffset > 0)
        totalFrames = std::max<int64_t>(
            {(int64_t)1, totalFrames - (int64_t)region.sampleEndOffset,
             (int64_t)voice.loop.end});

      // ---- block-rate modulation ----
      const double blockFrames = timings.length;
      double lfoVal = 0.;
      if(p.lfoDest != SamplerParams::LfoOff && p.lfoDepth > 0.f)
        lfoVal = voice.lfo.advanceBlock(blockFrames, p.lfoRate, p.lfoDelay, m_sampleRate);
      voice.pitchEnv.advanceBlock(blockFrames, p.pitchEnvDecay, m_sampleRate);
      const double glideSemis
          = voice.glide.advanceBlock(blockFrames, p.glide, m_sampleRate, p.glideType);

      const double bend = p.perChannel ? m_bendCh[voice.channel] : m_bend;
      double semis = glideSemis + voice.randomSemis + bend * p.bendRange
                     + voice.pitchEnv.value;
      if(p.lfoDest == SamplerParams::LfoPitch)
        semis += lfoVal * p.lfoDepth * 2.0; // up to +-2 semitones of vibrato
      // File-specified vibrato (SF2 vibLfo), on top of the user LFO
      if(region.vibLfoToPitch != 0.f)
        semis += voice.vibLfo.advanceBlock(
                     blockFrames, region.vibLfoFreq, region.vibLfoDelay, m_sampleRate)
                 * region.vibLfoToPitch / 100.0;

      // File-specified modLfo (tremolo / filter wobble / pitch) and
      // modulation envelope (SF2 EG2 -> pitch / cutoff)
      double fileFcCents = voice.velFcCents;
      double fileGain = 1.;
      if(region.modLfoToPitch != 0.f || region.modLfoToFc != 0.f
         || region.modLfoToVol != 0.f)
      {
        const double m = voice.modLfo.advanceBlock(
            blockFrames, region.modLfoFreq, region.modLfoDelay, m_sampleRate);
        semis += m * region.modLfoToPitch / 100.0;
        fileFcCents += m * region.modLfoToFc;
        if(region.modLfoToVol != 0.f)
          fileGain *= std::pow(10.0, m * -region.modLfoToVol / 200.0);
      }
      if(region.modEnvToPitch != 0.f || region.modEnvToFc != 0.f)
      {
        // Delay and hold folded into the neighboring stages (block rate)
        const double e = voice.fileModEnv.advanceBlock(
            blockFrames, region.eg2Delay + region.eg2Attack,
            region.eg2Hold + region.eg2Decay, region.eg2Sustain, region.eg2Release,
            m_sampleRate);
        semis += e * region.modEnvToPitch / 100.0;
        fileFcCents += e * region.modEnvToFc;
      }
      // gig: EG2 and LFO2 scale the cutoff in 0..127 units, i.e. take
      // cents away from the note's whole span
      if(voice.gigFcSpanCents > 0.)
      {
        const double e = voice.fileModEnv.advanceBlock(
            blockFrames, region.eg2Attack, region.eg2Decay, region.eg2Sustain,
            region.eg2Release, m_sampleRate);
        double x = 0.;
        if(region.gigLfo2Depth > 0.f)
          x = region.gigLfo2Depth * 0.5
              * (1. + voice.modLfo.advanceBlock(
                     blockFrames, region.gigLfo2Freq, 0., m_sampleRate));
        const double scale = std::max(e * (1. - x), 1e-3);
        fileFcCents += voice.gigFcSpanCents * (scale - 1.);
      }
      // gig: controller-driven layer crossfade
      if(region.xfadeSource >= 0)
        fileGain *= gigCrossfadeGain(region, m_cc[std::min(region.xfadeSource, 128)]);
      const double ratio = semitonesToRatio(semis);

      const double fenv = voice.filterEnv.advanceBlock(
          blockFrames, p.filterEnvAttack, p.filterEnvDecay, p.filterEnvSustain,
          p.filterEnvRelease, m_sampleRate);
      if(voice.filterOn)
      {
        double octaves = 4.0 * p.filterEnvAmount * fenv
                         + 2.0 * p.velToCutoff * (voice.velocity / 127.0)
                         + p.filterKeytrack * (voice.note - 60) / 12.0
                         + fileFcCents / 1200.0;
        if(p.lfoDest == SamplerParams::LfoCutoff)
          octaves += lfoVal * p.lfoDepth * 4.0;
        voice.filter.configure(
            voice.filterTypeEff, voice.cutoffBaseHz * std::exp2(octaves), voice.q,
            m_sampleRate, voice.filterGain);
      }

      double gainMod = fileGain;
      if(p.lfoDest == SamplerParams::LfoAmp)
        gainMod *= 1. - p.lfoDepth * 0.5 * (1. + lfoVal); // tremolo, downwards
      double panMod = 0.;
      if(p.lfoDest == SamplerParams::LfoPan)
        panMod = lfoVal * p.lfoDepth;

      const double pan
          = std::clamp(region.pan / 64.0 + p.pan + panMod, -1., 1.);
      // Constant-power law (SF2 / FluidSynth): centre is -3 dB per side,
      // not -6 dB, so centred and hard-panned regions balance correctly
      const double panTheta = (pan + 1.) * (M_PI / 4.);
      const double panL = std::cos(panTheta);
      const double panR = std::sin(panTheta);
      const double gainTotal = voice.gain * p.volume * gainMod;

      // Anti-click fadeout near the end of non-looping playback
      const int64_t fadeSamples = (int64_t)(m_sampleRate * 0.00075);

      // Choke fade: exclusive classes use a fast-but-audible 0.3 s
      // (FluidSynth forces a ~0.3 s release; a 10 ms chop was found too
      // abrupt on open hi-hats), mono/legato the Mono fade control
      const double chokeStep
          = voice.choked ? 1.0 / std::max(1.0, m_sampleRate * voice.chokeSeconds)
                         : 0.0;

      for(int64_t k = 0; k < timings.length; k++)
      {
        const auto sr = readPosition(
            voice.head.pos, totalFrames, voice.loop, xfadeFrames, voice.released);

        double sL, sR;
        {
          const int64_t i0 = sr.idx;
          const int64_t i1 = std::min(i0 + 1, totalFrames - 1);
          if(channels == 1)
          {
            double s = sampleData[0][i0]
                       + (sampleData[0][i1] - sampleData[0][i0]) * sr.frac;
            sL = sR = s;
          }
          else
          {
            sL = sampleData[0][i0]
                 + (sampleData[0][i1] - sampleData[0][i0]) * sr.frac;
            sR = sampleData[1][i0]
                 + (sampleData[1][i1] - sampleData[1][i0]) * sr.frac;
          }
          if(sr.xfadeWeight > 0.)
          {
            const double w = sr.xfadeWeight;
            const int64_t xi = sr.xfadeIdx;
            if(channels == 1)
            {
              sL = sL * (1. - w) + sampleData[0][xi] * w;
              sR = sL;
            }
            else
            {
              sL = sL * (1. - w) + sampleData[0][xi] * w;
              sR = sR * (1. - w) + sampleData[1][xi] * w;
            }
          }
        }

        // End fade only applies when playback can actually run off the end
        if(voice.loop.mode == 0 && fadeSamples > 0)
        {
          const int64_t remaining = voice.head.dir > 0
                                        ? totalFrames - sr.idx
                                        : sr.idx + 1;
          if(remaining < fadeSamples)
          {
            const double g = (double)remaining / (double)fadeSamples;
            sL *= g;
            sR *= g;
          }
        }

        if(voice.filterOn)
          voice.filter.process(sL, sR);
        voice.lofi.process(p.lofi, sL, sR);

        double aenv = voice.amp_adsr();
        if(voice.choked)
        {
          aenv *= voice.chokeGain;
          voice.chokeGain -= chokeStep;
          if(voice.chokeGain <= 0.)
          {
            stop_voice(voice);
            break;
          }
        }

        out_l[k] += sL * aenv * gainTotal * panL;
        out_r[k] += sR * aenv * gainTotal * panR;

        if(!voice.head.advance(ratio, voice.loop, totalFrames, voice.released))
        {
          stop_voice(voice);
          break;
        }
      }

      // The envelope of an ignored note-off went into its release: the other
      // envelopes follow, as on a note-off.
      if(voice.playing && !voice.released && voice.ignoresNoteOff
         && voice.amp_adsr.releasing())
      {
        voice.filterEnv.release();
        voice.fileModEnv.release();
        voice.released = true;
      }

      if(voice.playing && voice.amp_adsr.done())
        stop_voice(voice);
    }
  }

private:
public:
  struct UiNote
  {
    uint8_t note{}, velocity{};
    bool on{};
  };
  // MIDI keys physically held, per channel: gates the release-trigger scan
  // so stray or duplicate note-offs cannot fire it, and the mono lines
  std::bitset<128> m_held[16];
  // run() executes once per token request but the inlet message list is
  // cleared once per graph tick: track the consumed part of the buffer
  int64_t m_lastTick{-1};
  int64_t m_midiConsumedUntil{};
  std::vector<std::pair<int64_t, std::array<uint8_t, 3>>> m_midiScratch;
  std::vector<UiNote> m_uiNotes; // execution-thread only
  void queue_ui_note(int note, int velocity, bool on)
  {
    m_uiNotes.push_back(
        {(uint8_t)std::clamp(note, 0, 127), (uint8_t)std::clamp(velocity, 0, 127),
         on});
  }

private:

  // ---- voice modes ----
  //
  // Mono-style modes play "lines": one sounding note at a time, picked among
  // the line's held keys by the note priority. Mono / Legato (and fingered)
  // use one line, or one per MIDI channel; Duophonic two (lowest and highest
  // held key). The poly modes pair each new note with a previous one by
  // pitch (Poly legato: a held line moves to it; Poly glide: a new voice
  // glides from it), every pairing living in its own dynamic line.

  static constexpr int monoLines = 16;        // Mono family: 0..15
  static constexpr int duoLines = 16;         // Duophonic: 16..47, two each
  static constexpr int fixedLines = monoLines + 2 * duoLines;
  static constexpr int firstDynamicLine = 64; // poly modes: 64 and up

  // A key as a (channel, note) pair packed in 11 bits
  static constexpr int key(int ch, int note) noexcept
  {
    return ((ch & 15) << 7) | (note & 127);
  }
  static constexpr int keyChannel(int k) noexcept { return (k >> 7) & 15; }
  static constexpr int keyNote(int k) noexcept { return k & 127; }

  struct Line
  {
    // Held keys in the order they were struck, oldest first; entries whose
    // key is no longer held are skipped when read (m_held is the truth)
    uint16_t stack[128]{};
    int count{};
    int sounding{-1}; // key the line plays, -1 = none
    // Absolute pitch (in semitones, MIDI note scale) the line last reached:
    // where its next note glides from when nothing sounds on it any more
    double lastPitch{};
    bool hasPitch{};
  };

  // How start_voice starts a voice of a note
  struct VoiceStart
  {
    int line{-1};
    bool legatoTransfer{};  // continue a phrase: no attack
    bool glide{};           // glide from glideFrom
    double glideFrom{};     // absolute pitch
  };

  bool isHeld(int k) const noexcept { return m_held[keyChannel(k)].test(keyNote(k)); }

  void clear_keys() noexcept
  {
    for(auto& h : m_held)
      h.reset();
    for(auto& h : m_sounded)
      h.reset();
    for(auto& l : m_lines)
    {
      l.count = 0;
      l.sounding = -1;
    }
    m_pendingCount = 0;
  }

  void pushHeld(Line& line, int k) noexcept
  {
    int w = 0;
    for(int i = 0; i < line.count; i++)
      if(line.stack[i] != k && isHeld(line.stack[i]))
        line.stack[w++] = line.stack[i];
    line.count = w;
    if(line.count == 128) // full: forget the oldest
    {
      std::copy(line.stack + 1, line.stack + 128, line.stack);
      line.count--;
    }
    line.stack[line.count++] = (uint16_t)k;
  }

  // The held key a line plays by the note priority, -1 if none is held
  int pickHeld(const Line& line) const noexcept
  {
    int best = -1;
    for(int i = 0; i < line.count; i++)
    {
      const int k = line.stack[i];
      if(!isHeld(k))
        continue;
      switch(m_params.notePriority)
      {
        case SamplerParams::PriorityHighest:
          if(best < 0 || keyNote(k) >= keyNote(best))
            best = k;
          break;
        case SamplerParams::PriorityLowest:
          if(best < 0 || keyNote(k) <= keyNote(best))
            best = k;
          break;
        default: // last: the most recent wins
          best = k;
          break;
      }
    }
    return best;
  }

  int heldCount(const Line& line) const noexcept
  {
    int n = 0;
    for(int i = 0; i < line.count; i++)
      n += isHeld(line.stack[i]) ? 1 : 0;
    return n;
  }

  int lineGroup(int ch) const noexcept { return m_params.perChannel ? (ch & 15) : 0; }

  // Absolute pitch a voice sounds at (its note plus the glide still to go)
  static double voicePitch(const gig_voice& v) noexcept
  {
    return v.note + (v.glide.current - v.glide.target);
  }

  // The pitch-tracked voice a line sounds with, the newest if several
  const gig_voice* lineVoice(int line, bool heldOnly) const noexcept
  {
    const gig_voice* best = nullptr;
    for(const auto& v : m_voices)
      if(v.playing && !v.choked && v.line == line && v.region && v.region->pitchTrack
         && (!heldOnly || !v.released))
        if(!best || v.startOrder > best->startOrder)
          best = &v;
    return best;
  }

  void relabelLine(int from, int to) noexcept
  {
    for(auto& v : m_voices)
      if(v.line == from)
        v.line = to;
  }

  // Whether any region would sound this key: a key with no mapped region is
  // a no-op, and in particular must not choke what plays
  bool keyPlayable(const GigInstrument& instr, int note, int velocity) const noexcept
  {
    const int matchNote
        = m_params.chromatic ? std::clamp(m_params.chromaticRoot, 0, 127) : note;
    const int regionCount = std::min<int>(instr.regions.size(), max_regions);
    for(int i = 0; i < regionCount; i++)
    {
      auto& region = instr.regions[i];
      if(!region.muted && !region.releaseTrigger
         && regionMatches(region, matchNote, velocity))
        return true;
    }
    return false;
  }

  void note_on(GigInstrument& instr, int ch, int note, int velocity) noexcept
  {
    ch &= 15;
    note &= 127;
    // Struck again while pending: the earlier strike sounds first
    if(m_pendingCount > 0 && isPending(key(ch, note)))
      flush_pending(instr);
    m_held[ch].set(note);
    m_noteVelocity[ch][note] = (uint8_t)velocity;
    m_noteOnClock[ch][note] = m_clock;
    dispatch_note_on(instr, ch, note, velocity);
  }

  // A held key into the voice mode
  void dispatch_note_on(GigInstrument& instr, int ch, int note, int velocity) noexcept
  {
    // Poly modes: every key struck sounds (its release samples included,
    // even for a key only release samples map); mono-style lines mark the
    // keys they actually play
    const bool polyish = m_params.voiceMode == SamplerParams::Poly
                         || m_params.voiceMode == SamplerParams::PolyGlide
                         || m_params.voiceMode == SamplerParams::PolyLegato;
    m_sounded[ch].set(note, polyish);

    switch(m_params.voiceMode)
    {
      case SamplerParams::Poly:
        start_note(instr, ch, note, velocity, {});
        break;
      case SamplerParams::Duophonic: {
        auto& lo = m_lines[monoLines + 2 * lineGroup(ch)];
        pushHeld(lo, key(ch, note));
        update_duo(instr, lineGroup(ch));
        break;
      }
      case SamplerParams::PolyGlide:
      case SamplerParams::PolyLegato:
        // Gathered into a chord, paired when the chord window closes
        if(m_pendingCount == 0)
          m_pendingSince = m_clock;
        if(m_pendingCount < maxChord)
          m_pending[m_pendingCount++] = {(uint16_t)key(ch, note), (uint8_t)velocity};
        break;
      default: { // the Mono family
        const int l = lineGroup(ch);
        auto& line = m_lines[l];
        pushHeld(line, key(ch, note));
        // Note priority: a key that does not win only joins the held keys
        if(pickHeld(line) != key(ch, note))
          break;
        play_line(instr, l, ch, note, velocity, false);
        break;
      }
    }
  }

  void note_off(GigInstrument& instr, int ch, int note) noexcept
  {
    ch &= 15;
    note &= 127;
    if(m_pendingCount > 0 && isPending(key(ch, note)))
      flush_pending(instr);
    const bool wasHeld = m_held[ch].test(note);
    m_held[ch].reset(note);

    // Lines move on before the release below: a voice that moves to another
    // key changes its note and is not released
    if(wasHeld && !m_params.ignoreNoteOff)
    {
      switch(m_params.voiceMode)
      {
        case SamplerParams::Poly:
        case SamplerParams::PolyGlide:
        case SamplerParams::PolyLegato:
          break;
        case SamplerParams::Duophonic:
          update_duo(instr, lineGroup(ch));
          break;
        default: {
          // Releasing the sounding key while others are still held goes
          // back to the one the note priority picks - legato glides there
          // without a new attack, mono strikes it again
          const int l = lineGroup(ch);
          auto& line = m_lines[l];
          if(line.sounding == key(ch, note))
          {
            line.sounding = -1;
            if(const int back = pickHeld(line); back >= 0)
              play_line(
                  instr, l, keyChannel(back), keyNote(back),
                  m_noteVelocity[keyChannel(back)][keyNote(back)], true);
          }
          break;
        }
      }
    }

    // Poly legato gathering a chord: the lines of the keys released now
    // are what its notes continue - they are released when it is paired,
    // if no note took them over (flush_pending's sweep)
    const bool defer
        = m_params.voiceMode == SamplerParams::PolyLegato && m_pendingCount > 0;
    for(auto& voice : m_voices)
    {
      if(!voice.playing || voice.released)
        continue;
      if(voice.note != note || voice.channel != ch)
        continue;
      if(defer && voice.line >= firstDynamicLine)
        continue;
      keyUp(voice);
    }
    // Stray or duplicate note-offs (dropped note-on, reload, controllers
    // sending both vel-0 and note-off) must not fire release triggers, nor
    // note-offs that are ignored, nor keys a mono line never played
    const bool sounded = m_sounded[ch].test(note);
    m_sounded[ch].reset(note);
    if(!wasHeld || !sounded || m_params.ignoreNoteOff)
      return;

    // Release triggers: dedicated regions fired on note-off (gig)
    const int velocity = m_noteVelocity[ch][note];
    const int matchNote = m_params.chromatic
                              ? std::clamp(m_params.chromaticRoot, 0, 127)
                              : note;
    const int regionCount = std::min<int>(instr.regions.size(), max_regions);
    for(int i = 0; i < regionCount; i++)
    {
      auto& region = instr.regions[i];
      if(!region.releaseTrigger || region.muted)
        continue;
      if(!regionMatches(region, matchNote, velocity))
        continue;
      start_voice(region, ch, note, matchNote, velocity, true, {});
    }
  }

  // A voice's key went up
  static void keyUp(gig_voice& voice) noexcept
  {
    // The key is up, but a note started with note-offs ignored plays on.
    // Per note, not per mode: a note held while the mode switched to Ignore
    // must still stop when its key is released.
    if(!voice.playing || voice.released || voice.ignoresNoteOff)
      return;
    if(voice.oneShotEff)
    {
      // One-shots ignore note-off for the envelope, but a loop-until-
      // release sample must still be allowed to run to its end
      if(voice.loop.mode == 3)
        voice.released = true;
      return;
    }
    voice.amp_adsr.startRelease();
    voice.filterEnv.release();
    voice.fileModEnv.release();
    voice.released = true;
  }

  // Plays a key on a mono-style line. The line's current voices are taken
  // over (legato) or cut by the Mono fade (mono). returning: going back to a
  // held key, always connected.
  void play_line(
      GigInstrument& instr, int l, int ch, int note, int velocity,
      bool returning) noexcept
  {
    const auto& p = m_params;
    const bool legato = p.voiceMode == SamplerParams::Legato
                        || p.voiceMode == SamplerParams::LegatoFingered;
    const bool fingered = p.voiceMode == SamplerParams::MonoFingered
                          || p.voiceMode == SamplerParams::LegatoFingered;
    // Fingered modes: only a note struck while another key of the line is
    // held is connected to the previous one (glide, legato); a detached
    // note starts afresh
    const bool connected = !fingered || returning || heldCount(m_lines[l]) > 1;
    move_line(instr, l, ch, note, velocity, legato, connected, connected);
  }

  // Moves line l to a key. legato: a voice still held is retargeted (same
  // zone) or continued without attack (zone change); otherwise the line's
  // voices are cut by the Mono fade and the key struck. glide: from where
  // the line is (or last was).
  void move_line(
      GigInstrument& instr, int l, int ch, int note, int velocity, bool legato,
      bool connected, bool glide) noexcept
  {
    const auto& p = m_params;
    // A key with no region: the line stays where it is
    if(!keyPlayable(instr, note, velocity))
      return;
    // The poly modes' dynamic lines keep no state besides their voices
    Line scratch;
    auto& line = l >= 0 && l < fixedLines ? m_lines[l] : scratch;
    const int matchNote = p.chromatic ? std::clamp(p.chromaticRoot, 0, 127) : note;

    const gig_voice* held = lineVoice(l, true);
    const gig_voice* any = held ? held : lineVoice(l, false);
    const double from = any ? voicePitch(*any) : line.lastPitch;
    const bool haveFrom = any || line.hasPitch;

    if(legato && held && connected)
    {
      // Same zone still held: just glide there, no retrigger at all
      bool retargeted = false;
      for(auto& v : m_voices)
      {
        if(v.playing && !v.released && !v.choked && v.line == l && v.region
           && v.region->pitchTrack && matchNote >= v.region->keyLow
           && matchNote <= v.region->keyHigh)
        {
          const double target
              = basePitchSemitones(*v.region, p, matchNote) + (note - matchNote);
          v.glide.start(v.glide.current, target, true);
          v.note = note;
          v.channel = ch;
          v.startClock = m_clock; // the line moved: new for the chord guard
          retargeted = true;
        }
      }
      if(retargeted)
      {
        m_sounded[ch & 15].set(note & 127);
        line.sounding = key(ch, note);
        line.lastPitch = note;
        line.hasPitch = true;
        return;
      }
    }

    // Crossing into another zone (or mono retrigger): the line's previous
    // melodic voices are cut...
    for(auto& v : m_voices)
      if(v.playing && !v.choked && v.line == l && v.region && v.region->pitchTrack)
        choke(v, p.monoFade);

    // ...and in legato, while a note was still held, the replacement voice
    // continues the phrase: its attack is suppressed so the zone change
    // crossfades instead of re-articulating
    VoiceStart s;
    s.line = l;
    s.legatoTransfer = legato && held && connected;
    s.glide = glide && haveFrom;
    s.glideFrom = from;
    start_note(instr, ch, note, velocity, s);
    m_sounded[ch & 15].set(note & 127);
    line.sounding = key(ch, note);
    line.lastPitch = note;
    line.hasPitch = true;
  }

  // Duophonic: the low line plays the lowest held key, the high line the
  // highest; a single key sounds once, on the low line
  void update_duo(GigInstrument& instr, int group) noexcept
  {
    const int lo = monoLines + 2 * group, hi = lo + 1;
    auto& stack = m_lines[lo];
    int kLo = -1, kHi = -1;
    for(int i = 0; i < stack.count; i++)
    {
      const int k = stack.stack[i];
      if(!isHeld(k))
        continue;
      if(kLo < 0 || keyNote(k) < keyNote(kLo))
        kLo = k;
      if(kHi < 0 || keyNote(k) > keyNote(kHi))
        kHi = k;
    }
    if(kHi >= 0 && kLo >= 0 && keyNote(kHi) == keyNote(kLo))
      kHi = -1;

    auto& lineLo = m_lines[lo];
    auto& lineHi = m_lines[hi];
    // A line already playing a target keeps it, swapping sides if needed
    if(kLo >= 0 && lineHi.sounding == kLo && lineLo.sounding != kLo)
    {
      // The high voice becomes the low one
      relabelLine(lo, firstDynamicLine - 1); // park
      relabelLine(hi, lo);
      relabelLine(firstDynamicLine - 1, hi);
      std::swap(lineLo.sounding, lineHi.sounding);
      std::swap(lineLo.lastPitch, lineHi.lastPitch);
      std::swap(lineLo.hasPitch, lineHi.hasPitch);
    }
    else if(kHi >= 0 && lineLo.sounding == kHi && lineHi.sounding != kHi)
    {
      relabelLine(hi, firstDynamicLine - 1);
      relabelLine(lo, hi);
      relabelLine(firstDynamicLine - 1, lo);
      std::swap(lineLo.sounding, lineHi.sounding);
      std::swap(lineLo.lastPitch, lineHi.lastPitch);
      std::swap(lineLo.hasPitch, lineHi.hasPitch);
    }

    const auto follow = [&](int l, int k) {
      auto& line = m_lines[l];
      if(k < 0)
      {
        // Nothing for this side: what it played is released with its key,
        // or, if its key is still held, belongs to the other side now
        if(line.sounding >= 0 && isHeld(line.sounding))
        {
          for(auto& v : m_voices)
            if(v.playing && !v.released && v.line == l)
            {
              v.amp_adsr.startRelease();
              v.filterEnv.release();
              v.fileModEnv.release();
              v.released = true;
            }
        }
        line.sounding = -1;
        return;
      }
      if(line.sounding == k)
        return;
      // The side moves while it still sounds a held note: a legato step;
      // from silence: a fresh note
      const bool moving = line.sounding >= 0 && lineVoice(l, true);
      move_line(
          instr, l, keyChannel(k), keyNote(k),
          m_noteVelocity[keyChannel(k)][keyNote(k)], true, moving, moving);
    };
    follow(lo, kLo);
    follow(hi, kHi);
  }

  // ---- poly modes: chord gathering and pitch-order pairing ----

  static constexpr int maxChord = 64;
  struct Pending
  {
    uint16_t key;
    uint8_t velocity;
  };

  bool isPending(int k) const noexcept
  {
    for(int i = 0; i < m_pendingCount; i++)
      if(m_pending[i].key == k)
        return true;
    return false;
  }

  // Pairs the sorted new notes with the sorted previous ones without
  // crossing: the most pairs first, then the smallest total distance;
  // pairs further apart than the max interval are not allowed. out[i] is
  // the index of the source note i is paired with, or -1.
  void pairByPitch(
      const double* notes, int n, const double* sources, int m, double maxInterval,
      int* out) noexcept
  {
    // dp over prefixes (<= maxChord x maxChord), in the node rather than on
    // the audio thread's stack
    static constexpr double big = 1e6;
    auto& dp = m_pairing.dp;
    auto& how = m_pairing.how; // 0 skip note, 1 skip source, 2 pair
    for(int i = 0; i <= n; i++)
      for(int j = 0; j <= m; j++)
      {
        if(i == 0 || j == 0)
        {
          dp[i][j] = 0.;
          how[i][j] = i == 0 ? 1 : 0;
          continue;
        }
        double best = dp[i - 1][j];
        uint8_t h = 0;
        if(dp[i][j - 1] > best)
        {
          best = dp[i][j - 1];
          h = 1;
        }
        const double d = std::abs(notes[i - 1] - sources[j - 1]);
        if(d <= maxInterval && dp[i - 1][j - 1] + big - d > best)
        {
          best = dp[i - 1][j - 1] + big - d;
          h = 2;
        }
        dp[i][j] = best;
        how[i][j] = h;
      }
    for(int i = 0; i < n; i++)
      out[i] = -1;
    int i = n, j = m;
    while(i > 0 && j > 0)
    {
      if(how[i][j] == 2)
      {
        out[i - 1] = j - 1;
        i--;
        j--;
      }
      else if(how[i][j] == 1)
        j--;
      else
        i--;
    }
  }

  // A dynamic line id; wraps (long after any line using it has ended)
  int newLine() noexcept
  {
    const int l = m_nextLine;
    m_nextLine = m_nextLine == std::numeric_limits<int>::max() ? firstDynamicLine
                                                               : m_nextLine + 1;
    return l;
  }

  void flush_pending(GigInstrument& instr) noexcept
  {
    const auto& p = m_params;
    const int n = m_pendingCount;
    m_pendingCount = 0;
    if(n == 0)
      return;
    Pending chord[maxChord];
    std::copy(m_pending, m_pending + n, chord);
    // The last struck (mapped) key per line group, for Poly glide's "last
    // note" source
    int lastStruck[16];
    std::fill(std::begin(lastStruck), std::end(lastStruck), -1);
    for(int i = 0; i < n; i++)
      if(keyPlayable(instr, keyNote(chord[i].key), chord[i].velocity))
        lastStruck[lineGroup(keyChannel(chord[i].key))] = keyNote(chord[i].key);

    if(p.voiceMode != SamplerParams::PolyGlide && p.voiceMode != SamplerParams::PolyLegato)
    {
      // The mode changed while the chord gathered: play it as it is now
      for(int i = 0; i < n; i++)
        if(isHeld(chord[i].key))
          dispatch_note_on(
              instr, keyChannel(chord[i].key), keyNote(chord[i].key), chord[i].velocity);
      releaseKeysUp();
      return;
    }
    const bool legato = p.voiceMode == SamplerParams::PolyLegato;

    std::sort(chord, chord + n, [](const Pending& a, const Pending& b) {
      return keyNote(a.key) < keyNote(b.key);
    });

    // Sources: one per line, the newest voice of each. Poly legato: lines
    // still held (legato only joins overlapping notes). Poly glide: anything
    // still sounding. Lines started within the chord guard are chord mates.
    const int64_t guard
        = p.chordWindow > 0.f ? 0 : (int64_t)(0.025 * m_sampleRate);
    struct Source
    {
      int line;
      int key;
      double pitch;
    };
    Source sources[maxChord];
    int m = 0;
    if(!legato && p.polyGlideFrom == SamplerParams::GlideFromLast)
    {
      // One per line group: the last note struck there
      for(int g = 0; g < 16; g++)
        if(m_polyHasLast[g])
          sources[m++] = {-1, key(g, 0), m_polyLastPitch[g]};
    }
    else
    {
      for(const auto& v : m_voices)
      {
        if(!v.playing || v.choked || v.line < firstDynamicLine || !v.region
           || !v.region->pitchTrack)
          continue;
        // Poly legato: lines of held keys, or of keys released while the
        // chord gathered (their release waits for this pairing); not notes
        // that ignore their key
        if(legato
           && (v.released
               || (!isHeld(key(v.channel, v.note))
                   && (v.ignoresNoteOff || v.oneShotEff))))
          continue;
        if(m_clock - v.startClock < guard)
          continue;
        bool seen = false;
        for(int s = 0; s < m && !seen; s++)
          seen = sources[s].line == v.line;
        if(seen || m == maxChord)
          continue;
        sources[m++] = {v.line, key(v.channel, v.note), voicePitch(v)};
      }
      std::sort(sources, sources + m, [](const Source& a, const Source& b) {
        return a.pitch < b.pitch;
      });
    }

    // Pairing, within each channel when lines are per channel
    int pair[maxChord];
    std::fill(pair, pair + n, -1);
    const double maxInterval = std::max(0, p.maxInterval);
    if(!legato && p.polyGlideFrom == SamplerParams::GlideFromLast)
    {
      // Every note glides from the last note struck in its line group
      for(int i = 0; i < n; i++)
        for(int s = 0; s < m; s++)
          if(keyChannel(sources[s].key) == lineGroup(keyChannel(chord[i].key))
             && std::abs(keyNote(chord[i].key) - sources[s].pitch) <= maxInterval)
            pair[i] = s;
    }
    else
    {
      for(int ch = 0; ch < (p.perChannel ? 16 : 1); ch++)
      {
        double nPitch[maxChord], sPitch[maxChord];
        int nIdx[maxChord], sIdx[maxChord], nn = 0, mm = 0;
        for(int i = 0; i < n; i++)
          if(!p.perChannel || keyChannel(chord[i].key) == ch)
          {
            nPitch[nn] = keyNote(chord[i].key);
            nIdx[nn++] = i;
          }
        for(int s = 0; s < m; s++)
          if(!p.perChannel || keyChannel(sources[s].key) == ch)
          {
            sPitch[mm] = sources[s].pitch;
            sIdx[mm++] = s;
          }
        if(nn == 0 || mm == 0)
          continue;
        int out[maxChord];
        pairByPitch(nPitch, nn, sPitch, mm, maxInterval, out);
        for(int i = 0; i < nn; i++)
          if(out[i] >= 0)
            pair[nIdx[i]] = sIdx[out[i]];
      }
    }

    for(int i = 0; i < n; i++)
    {
      const int ch = keyChannel(chord[i].key), note = keyNote(chord[i].key);
      const int vel = chord[i].velocity;
      if(pair[i] >= 0 && legato)
      {
        // The held line moves to this note
        move_line(instr, sources[pair[i]].line, ch, note, vel, true, true, true);
        continue;
      }
      if(!keyPlayable(instr, note, vel))
        continue;
      VoiceStart s;
      s.line = newLine();
      if(pair[i] >= 0)
      {
        s.glide = true;
        s.glideFrom = sources[pair[i]].pitch;
      }
      start_note(instr, ch, note, vel, s);
    }
    for(int g = 0; g < 16; g++)
      if(lastStruck[g] >= 0)
      {
        m_polyLastPitch[g] = lastStruck[g];
        m_polyHasLast[g] = true;
      }
    // Notes released while the chord gathered (still sounding, briefly), and
    // lines whose key was released meanwhile and that no note took over
    releaseKeysUp();
  }

  // Releases the poly lines whose key is up (deferred while a chord gathered)
  void releaseKeysUp() noexcept
  {
    for(auto& v : m_voices)
      if(v.playing && !v.released && v.line >= firstDynamicLine
         && !isHeld(key(v.channel, v.note)))
        keyUp(v);
  }

  // Starts every region a key plays (chokes, round-robin / random picks)
  void start_note(
      GigInstrument& instr, int ch, int note, int velocity, const VoiceStart& s) noexcept
  {
    const auto& p = m_params;

    // Chromatic mode: every key plays what is mapped at the root, repitched
    const int matchNote = p.chromatic ? std::clamp(p.chromaticRoot, 0, 127) : note;

    const int regionCount = std::min<int>(instr.regions.size(), max_regions);

    if(!keyPlayable(instr, note, velocity))
      return;

    // Note-offs ignored: a new strike of a key releases the previous one, or
    // a sustaining sound would pile up forever. One-shots overlap as in the
    // Release mode.
    if(p.ignoreNoteOff)
      for(auto& v : m_voices)
        if(v.playing && !v.released && !v.oneShotEff && v.note == note
           && v.channel == ch)
        {
          v.amp_adsr.startRelease();
          v.filterEnv.release();
          v.fileModEnv.release();
          v.released = true;
        }

    // Chokes: any group this hit triggers cuts what currently sounds in it
    for(int i = 0; i < regionCount; i++)
    {
      auto& region = instr.regions[i];
      if(region.muted || region.releaseTrigger || region.chokeGroup < 0)
        continue;
      if(!regionMatches(region, matchNote, velocity))
        continue;
      for(auto& v : m_voices)
        if(v.playing && !v.choked && v.region
           && v.region->chokeGroup == region.chokeGroup)
          choke(v, 0.3);
    }

    // Zone matching with round-robin/random alternation. The loader
    // precomputed the alternation groups (assignAlternationGroups), so the
    // scan is linear; a small per-event cache keeps one pick per group.
    struct GroupPick
    {
      int group;
      int pick;
    };
    GroupPick picks[32];
    int pickCount = 0;

    for(int i = 0; i < regionCount; i++)
    {
      auto& region = instr.regions[i];
      if(region.muted || region.releaseTrigger)
        continue;
      if(!regionMatches(region, matchNote, velocity))
        continue;

      int policy = 0;
      switch(p.roundRobin)
      {
        case SamplerParams::RRFromFile:
          policy = region.selectionAlgo == 1   ? SamplerParams::RRCycle
                   : region.selectionAlgo == 2 ? SamplerParams::RRRandom
                                               : 0;
          break;
        case SamplerParams::RROff:
          policy = 0;
          break;
        case SamplerParams::RRCycle:
        case SamplerParams::RRRandom:
          policy = p.roundRobin;
          break;
      }

      if(policy == 0 || region.altGroup < 0 || region.altCount <= 1)
      {
        start_voice(region, ch, note, matchNote, velocity, false, s);
        continue;
      }

      int pick = -1;
      for(int k = 0; k < pickCount; k++)
        if(picks[k].group == region.altGroup)
        {
          pick = picks[k].pick;
          break;
        }
      if(pick < 0)
      {
        pick = pickAlternative(
            region.altCount, policy, m_rrCounter[note & 127], m_rngState);
        if(pickCount < 32)
          picks[pickCount++] = {region.altGroup, pick};
      }

      if(region.altIndex == pick)
        start_voice(region, ch, note, matchNote, velocity, false, s);
    }
  }

  void choke(gig_voice& v, double seconds) noexcept
  {
    v.choked = true;
    v.chokeSeconds = std::max(0., seconds);
  }

  bool regionMatches(const GigRegion& r, int note, int velocity) const noexcept
  {
    if(note < r.keyLow || note > r.keyHigh)
      return false;
    return velocityZoneGain(r, velocity, m_params.velXfade) > 0.;
  }

  gig_voice& allocate_voice() noexcept
  {
    const int cap = std::clamp(m_params.polyphony, 1, max_voices);
    int active = 0;
    for(auto& v : m_voices)
      if(v.playing)
        active++;

    if(active < cap)
      for(auto& v : m_voices)
        if(!v.playing)
          return v;

    // At the cap (or full pool): steal the oldest playing voice
    gig_voice* oldest = nullptr;
    for(auto& v : m_voices)
      if(v.playing && (!oldest || v.startOrder < oldest->startOrder))
        oldest = &v;
    return oldest ? *oldest : m_voices[0];
  }

  void start_voice(
      const GigRegion& region, int ch, int note, int matchNote, int velocity,
      bool fromRelease, const VoiceStart& s) noexcept
  {
    const bool legatoTransfer = s.legatoTransfer;
    if(!region.sample.data || region.sample.data->empty()
       || (*region.sample.data)[0].empty())
      return;
    const int64_t frames = std::ssize((*region.sample.data)[0]);
    const auto& p = m_params;

    // gig gains that can silence the voice entirely: decided before a voice
    // is taken, as taking one may steal a sounding note
    double gigGain = 1.;
    if(region.xfadeSource == -2)
      gigGain *= gigCrossfadeGain(
          region, region.forcedVelocity >= 0 ? region.forcedVelocity : velocity);
    // gig release triggers fade with the length of the note they end
    if(fromRelease && region.releaseTriggerDecay > 0.)
    {
      const double held
          = double(m_clock - m_noteOnClock[ch & 15][note & 127]) / m_sampleRate;
      gigGain *= std::max(0., 1. - region.releaseTriggerDecay * held);
    }
    if(gigGain <= 0.)
      return;

    auto& voice = allocate_voice();
    voice.region = &region;
    voice.note = note;
    voice.channel = ch & 15;
    voice.line = s.line;
    voice.startClock = m_clock;
    voice.velocity = velocity;
    voice.startOrder = m_voiceCounter++;
    voice.playing = true;
    voice.released = false;
    voice.choked = false;
    voice.chokeGain = 1.0;
    voice.oneShotEff = region.oneShot || fromRelease;
    voice.ignoresNoteOff = p.ignoreNoteOff;

    // Effective key / velocity: the SF2 keynum / velocity generators pin
    // them for pitch and gain purposes while matching and note-off keep the
    // played values
    const int pitchKey = region.forcedKey >= 0 ? region.forcedKey : note;
    const int pitchMatch = region.forcedKey >= 0 ? region.forcedKey : matchNote;
    const int effVel = region.forcedVelocity >= 0 ? region.forcedVelocity : velocity;

    // Pitch: static part (matched note for keytracking, plus the chromatic
    // distance to the played note) + glide start; live modulation per block
    const double base
        = basePitchSemitones(region, p, pitchMatch) + (pitchKey - pitchMatch);
    voice.randomSemis = region.randomPitch > 0
                            ? region.randomPitch * (2.0 * random01() - 1.0)
                            : 0.;
    // Glide from an absolute pitch: base is the pitch of `note` on this
    // region, whatever its root
    const bool glideOn = s.glide && p.glide > 0.f && region.pitchTrack;
    voice.glide.start(base + (s.glideFrom - note), base, glideOn);
    voice.pitchEnv.value = 0.;
    if(p.pitchEnvAmount != 0.f)
      voice.pitchEnv.trigger(
          p.pitchEnvAmount * velPitchEnvScale(p.velToPitchEnv, effVel));

    // Start position: region offset + global/velocity start offset
    const double startFrac = std::clamp(
        p.startOffset + p.velToStart * (1.0 - effVel / 127.0), 0., 0.95);
    int64_t startFrames
        = region.sampleStartOffset + (int64_t)(startFrac * frames);
    startFrames = std::clamp<int64_t>(startFrames, 0, frames - 1);
    if(p.reverse)
    {
      voice.head.pos = (double)(frames - 1 - startFrames);
      voice.head.dir = -1;
    }
    else
    {
      voice.head.pos = (double)startFrames;
      voice.head.dir = 1;
    }

    voice.loop = resolveLoop(region, p, frames);
    // A one-shot voice ignores note-off: an infinite loop would make it
    // immortal (loop-until-release stays valid, note-off lets it run out).
    // A release-trigger voice never gets any note-off at all.
    if(voice.oneShotEff && (voice.loop.mode == 1 || voice.loop.mode == 2))
      voice.loop.mode = 0;
    if(fromRelease)
      voice.loop.mode = 0;
    // Note-offs ignored: a loop until the release would never be left. Other
    // loops play on and the envelope ends the note (a sampled piano fades out
    // over its loop); as a one-shot it would stop at the end of its attack.
    if(p.ignoreNoteOff && voice.loop.mode == 3)
      voice.loop.mode = 0;

    // Gain: velocity curve, zone crossfade, region attenuation
    voice.gain = region.sampleAttenuation * velocityGain(region, p, effVel)
                 * velocityZoneGain(region, effVel, p.velXfade);
    voice.gain *= gigGain;

    // Default SF2 velocity->cutoff modulator (can be overridden or zeroed by
    // the bank): active for velocities >= 64, in cents of cutoff change
    voice.velFcCents = 0.;
    if(region.velToFcCents != 0.f && effVel >= 64)
      voice.velFcCents = region.velToFcCents * (1.0 - effVel / 127.0);

    // Amplitude envelope (per-sample); global override wins
    const auto env = resolveEnvelope(region, p);
    voice.amp_adsr.reset();
    voice.amp_adsr.set_sample_rate(m_sampleRate);
    // A legato zone transfer continues a held phrase: crossfade in instead
    // of re-articulating the attack
    const double attack
        = legatoTransfer && region.pitchTrack ? 0.003 : std::max(0.001, env.attack);
    voice.amp_adsr.attack(attack);
    voice.amp_adsr.delay(region.eg1Delay);
    // High notes hold and decay faster (SF2 keynumToVolEnv*, timecents per
    // key relative to key 60)
    double hold = region.eg1Hold;
    if(region.keynumToHold != 0.f && hold > 0.)
      hold = std::clamp(
          hold * std::exp2(region.keynumToHold * (60 - note) / 1200.0), 0., 120.0);
    voice.amp_adsr.hold(hold);
    double decay = std::max(0.001, env.decay);
    if(region.keynumToDecay != 0.f)
      decay = std::clamp(
          decay * std::exp2(region.keynumToDecay * (60 - note) / 1200.0), 0.001,
          120.0);
    voice.amp_adsr.decay(decay);
    voice.amp_adsr.sustain(env.sustain);
    // 16 ms floor like FluidSynth: shorter releases click
    voice.amp_adsr.release(std::max(0.016, env.release));
    // The EMU dB-slope time semantics only apply to the file's own envelope;
    // user-set knobs mean time-to-sustain / time-to-silence
    voice.amp_adsr.dbMode(region.eg1DbSlope && p.decay < 0.f && p.release < 0.f);
    // Note-offs ignored: the envelope does its release by itself after the
    // decay (attack, hold, decay, release), so the Release setting still
    // shapes the end of the note and a sustaining sound still ends. A
    // one-shot already plays its whole sample whatever the mode.
    voice.amp_adsr.oneShot(p.ignoreNoteOff && !voice.oneShotEff);
    // gig EG1 pre-attack level and held-note decay, when the file's envelope
    // is in use
    if(p.attack < 0.f)
      voice.amp_adsr.preAttack(region.eg1PreAttack);
    if(p.decay < 0.f && p.sustain < 0.f)
      voice.amp_adsr.decay2(region.eg1Decay2);
    voice.amp_adsr.amp(1.0);

    // Filter
    voice.filter.reset();
    voice.gigFcSpanCents = 0.;
    switch(p.filterType)
    {
      case SamplerParams::FilterFromFile:
        // The default velocity->cutoff modulator needs a running filter even
        // when the region declares none (base = wide open then)
        voice.filterOn = region.vcfEnabled || voice.velFcCents != 0.;
        voice.filterTypeEff = region.vcfEnabled ? region.vcfType
                                                : (int)SamplerParams::FilterLowpass;
        if(!region.vcfEnabled)
          voice.cutoffBaseHz = 19912.; // 13500 cents, the SF2 open default
        else if(region.vcfCutoffHz >= 0.f)
          voice.cutoffBaseHz = region.vcfCutoffHz;
        else
        {
          // gig: velocity factor and keyboard tracking scale the cutoff
          double units = region.vcfCutoff;
          if(region.vcfVelTable)
            units *= (*region.vcfVelTable)[std::clamp(effVel, 0, 127)];
          if(region.vcfKeytrackBreakpoint >= 0)
            units *= std::exp2((pitchKey - region.vcfKeytrackBreakpoint) / 12.0);
          units = std::clamp(units, 0., 127.);
          voice.cutoffBaseHz = gigCutoffToHz(units);
          if(region.gigCutoffEnv)
            voice.gigFcSpanCents = units * gigCentsPerCutoffUnit;
        }
        if(region.vcfQCb >= 0.f)
        {
          // SF2 convention: q in dB = cB/10 - 3.01 (so Q = 0 has no
          // resonance hump), and half the peak height is taken out of the
          // passband via the gain compensation
          const double qDb = region.vcfQCb / 10.0 - 3.01;
          voice.q = std::pow(10.0, qDb / 20.0);
          voice.filterGain = 1.0 / std::sqrt(std::max(voice.q, 0.1));
        }
        else
        {
          voice.q = 1.0 + region.vcfResonance * 9.0 / 127.0;
          voice.filterGain = 1.0;
        }
        break;
      case SamplerParams::FilterOff:
        voice.filterOn = false;
        break;
      default:
        voice.filterOn = true;
        voice.filterTypeEff = p.filterType;
        voice.cutoffBaseHz = p.cutoff;
        voice.q = 0.5 + p.resonance * 9.5;
        voice.filterGain = 1.0;
        break;
    }
    if(voice.filterOn)
      voice.filter.configure(
          voice.filterTypeEff, voice.cutoffBaseHz, voice.q, m_sampleRate,
          voice.filterGain);

    voice.filterEnv = {};
    voice.filterEnv.trigger();
    voice.fileModEnv = {};
    voice.fileModEnv.trigger(voice.gigFcSpanCents > 0. ? region.eg2PreAttack : 0.);
    voice.lfo = {};
    voice.vibLfo = {};
    voice.modLfo = {};
    voice.lofi = {};
  }

  // xorshift32: cheap allocation-free RNG for per-hit humanization
  double random01() noexcept
  {
    m_rngState ^= m_rngState << 13;
    m_rngState ^= m_rngState >> 17;
    m_rngState ^= m_rngState << 5;
    return m_rngState * (1.0 / 4294967296.0);
  }

  static void stop_voice(gig_voice& voice) noexcept
  {
    voice.region = nullptr;
    voice.note = -1;
    voice.playing = false;
    voice.released = false;
    voice.choked = false;
    voice.chokeGain = 1.0;
    voice.head = {};
  }

public:
  ossia::execution_state& m_st;
  std::shared_ptr<GigFileInfo> m_gigInfo;
  ossia::midi_inlet* midi_in{};
  ossia::value_inlet* control_ins[ControlCount]{};
  ossia::audio_outlet* audio_out{};
  std::vector<gig_voice> m_voices;
  SamplerParams m_params;

  struct TimeRaw
  {
    float x{};
    bool sync{};
    bool is_vec{};
  };
  TimeRaw m_timeRaw[ControlCount]{};
  bool m_envFromFile{true};
  float m_sustainRaw{1.f};
  bool m_sustainIsSentinel{};

  void resolve_sustain() noexcept
  {
    m_params.sustain
        = (m_envFromFile || m_sustainIsSentinel) ? -1.f : m_sustainRaw;
  }
  double m_tempo{ossia::root_tempo};

  // Effective control values, read from the UI thread by the feedback timer.
  // Benign torn reads, like the other executors' control feedback.
  float m_feedback[ControlCount][2]{};

  double m_sampleRate{48000.0};
  double m_bend{};
  int64_t m_voiceCounter{};
  uint32_t m_rngState{0x9E3779B9};
  uint8_t m_rrCounter[128]{};
  uint8_t m_noteVelocity[16][128]{};
  int64_t m_noteOnClock[16][128]{};
  Line m_lines[fixedLines];
  int m_nextLine{firstDynamicLine};
  struct PairingScratch
  {
    double dp[maxChord + 1][maxChord + 1];
    uint8_t how[maxChord + 1][maxChord + 1];
  } m_pairing{};
  // Poly modes: the chord gathering until the chord window closes
  Pending m_pending[maxChord]{};
  int m_pendingCount{};
  int64_t m_pendingSince{};
  double m_polyLastPitch[16]{}; // per line group
  bool m_polyHasLast[16]{};
  double m_bendCh[16]{}; // per-channel pitch bend, for per-channel lines
  int64_t m_clock{};
  uint8_t m_cc[129]{}; // CC values, then channel pressure
  // Keys that sounded since struck: gates their release triggers, so that a
  // key a mono line never played (note priority, duophonic inner keys) does
  // not fire its release samples
  std::bitset<128> m_sounded[16];
};

Component::Component(
    Deuterium::Gig::ProcessModel& proc, const ::Execution::Context& ctx,
    QObject* parent)
    : ::Execution::ProcessComponent_T<Deuterium::Gig::ProcessModel, ossia::node_process>{
        proc, ctx, "GigSamplerComponent", parent}
{
  std::shared_ptr<gigsampler_node> node
      = ossia::make_node<gigsampler_node>(*ctx.execState, *ctx.execState);

  if(auto gi = proc.gigInfo())
    node->reload(std::move(gi), ctx.execState->sampleRate);

  this->node = node;

  m_ossia_process = std::make_shared<ossia::node_process>(node);

  // Control inlets: initial values now, updates through the command queue.
  // setupExecution registers the control's type and domain on the execution
  // port, which is what lets the graph map modulation sources (LFOs,
  // automations) onto the control's range.
  const auto& inlets = proc.inlets();
  for(int i = 0; i < ControlCount && 1 + i < std::ssize(inlets); i++)
  {
    auto* ctl = qobject_cast<Process::ControlInlet*>(inlets[1 + i]);
    if(!ctl)
      continue;
    node->set_control(i, ctl->value());
    ctl->setupExecution(*node->control_ins[i], this);
    connect(
        ctl, &Process::ControlInlet::valueChanged, this,
        [this, node, i](const ossia::value& v) {
      in_exec([node, i, v] { node->set_control(i, v); });
    });
  }

  // Control feedback: periodically reflect the effective execution-side
  // values (including graph modulation) back onto the inlets so the UI
  // widgets can display them.
  std::weak_ptr<gigsampler_node> weak_node = node;
  connect(
      &proc, &Deuterium::Gig::ProcessModel::uiNoteTriggered, this,
      [this, weak_node](int note, int velocity, bool on) {
    if(auto node = weak_node.lock())
      in_exec([node, note, velocity, on] { node->queue_ui_note(note, velocity, on); });
  });
  con(ctx.doc.coarseUpdateTimer, &QTimer::timeout, this,
      [weak_node, proc = QPointer<Deuterium::Gig::ProcessModel>{&proc}] {
    auto node = weak_node.lock();
    // The model can be deleted before this component during teardown
    if(!node || !proc)
      return;
    const auto& inlets = proc->inlets();
    for(int i = 0; i < ControlCount && 1 + i < std::ssize(inlets); i++)
    {
      auto* ctl = qobject_cast<Process::ControlInlet*>(inlets[1 + i]);
      if(!ctl)
        continue;
      const float a = node->m_feedback[i][0];
      const float b = node->m_feedback[i][1];
      if(dynamic_cast<Process::TimeChooser*>(ctl))
        ctl->setExecutionValue(ossia::vec2f{a, b});
      else if(i == EnvFromFile || i == NoteOff) // bool-valued combo boxes
        ctl->setExecutionValue(a != 0.f);
      else if(dynamic_cast<Process::Toggle*>(ctl))
        ctl->setExecutionValue(a != 0.f);
      else if(
          dynamic_cast<Process::IntSlider*>(ctl)
          || dynamic_cast<Process::ComboBox*>(ctl))
        ctl->setExecutionValue(int(a));
      else
        ctl->setExecutionValue(a);
    }
  });

  connect(&proc, &Deuterium::Gig::ProcessModel::fileChanged, this, [this, node] {
    // gi may be null (load failed): reload with an empty file to stop playback
    auto gi = this->process().gigInfo();
    auto rate = this->system().execState->sampleRate;
    in_exec([node, gi = std::move(gi), rate]() mutable {
      node->reload(std::move(gi), rate);
    });
  });
}

Component::~Component() { }

}
}
