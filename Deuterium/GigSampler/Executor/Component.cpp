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
#include <cmath>

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
  Lfo vibLfo; // file-specified vibrato (SF2 vibLfo generators)
  PitchEnv pitchEnv;
  BlockEnv filterEnv;
  GlideState glide;

  int note{-1};
  int velocity{};
  int64_t startOrder{};
  bool playing{};
  bool released{};
  bool choked{};
  bool oneShotEff{};
  bool filterOn{};
  int filterTypeEff{};
  double cutoffBaseHz{20000.};
  double q{1.};
  double filterGain{1.}; // passband compensation for file-driven resonance
  double chokeGain{1.};
  double gain{1.};       // velocity * zone crossfade * region attenuation
  double randomSemis{};
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
    m_haveLastPitch = false;
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
    applySamplerControl(m_params, control, v);
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
      case LfoDelay:
        m_params.lfoDelay = secs;
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

    if(!m_gigInfo)
      return;
    if(m_gigInfo->selectedInstrument < 0
       || m_gigInfo->selectedInstrument >= std::ssize(m_gigInfo->instruments))
      return;

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

    for(auto& mess : this->midi_in->data.messages)
    {
      // Inlet data persists across the run() calls of one cycle: only
      // consume the messages stamped inside this token's window
      const auto ts = (int64_t)mess.timestamp;
      if(ts < timings.start_sample || ts >= timings.start_sample + timings.length)
        continue;

      uint8_t data[4];
      int n = cmidi2_convert_single_ump_to_midi1((uint8_t*)data, 3, mess.data);
      if(n != 3)
        continue;

      libremidi::message m{{data[0], data[1], data[2]}};
      const int note = data[1];
      const int velocity = data[2];

      switch(m.get_message_type())
      {
        case libremidi::message_type::NOTE_ON: {
          if(velocity == 0)
            goto note_off;
          note_on(instr, note, velocity);
          break;
        }
        note_off:
        case libremidi::message_type::NOTE_OFF: {
          note_off(instr, note);
          break;
        }
        case libremidi::message_type::PITCH_BEND: {
          const int v14 = data[1] | (data[2] << 7);
          m_bend = (v14 - 8192) / 8192.0;
          break;
        }
        default:
          break;
      }
    }

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
        continue;
      auto& sampleData = *region.sample.data;
      const int channels = sampleData.size();
      if(channels == 0)
        continue;

      const int64_t totalFrames = std::ssize(sampleData[0]);

      // ---- block-rate modulation ----
      const double blockFrames = timings.length;
      double lfoVal = 0.;
      if(p.lfoDest != SamplerParams::LfoOff && p.lfoDepth > 0.f)
        lfoVal = voice.lfo.advanceBlock(blockFrames, p.lfoRate, p.lfoDelay, m_sampleRate);
      voice.pitchEnv.advanceBlock(blockFrames, p.pitchEnvDecay, m_sampleRate);
      const double glideSemis = voice.glide.advanceBlock(blockFrames, p.glide, m_sampleRate);

      double semis = glideSemis + voice.randomSemis + m_bend * p.bendRange
                     + voice.pitchEnv.value;
      if(p.lfoDest == SamplerParams::LfoPitch)
        semis += lfoVal * p.lfoDepth * 2.0; // up to +-2 semitones of vibrato
      // File-specified vibrato (SF2 vibLfo), on top of the user LFO
      if(region.vibLfoToPitch != 0.f)
        semis += voice.vibLfo.advanceBlock(
                     blockFrames, region.vibLfoFreq, region.vibLfoDelay, m_sampleRate)
                 * region.vibLfoToPitch / 100.0;
      const double ratio = semitonesToRatio(semis);

      const double fenv = voice.filterEnv.advanceBlock(
          blockFrames, p.filterEnvAttack, p.filterEnvDecay, p.filterEnvSustain,
          p.filterEnvRelease, m_sampleRate);
      if(voice.filterOn)
      {
        double octaves = 4.0 * p.filterEnvAmount * fenv
                         + 2.0 * p.velToCutoff * (voice.velocity / 127.0)
                         + p.filterKeytrack * (voice.note - 60) / 12.0;
        if(p.lfoDest == SamplerParams::LfoCutoff)
          octaves += lfoVal * p.lfoDepth * 4.0;
        voice.filter.configure(
            voice.filterTypeEff, voice.cutoffBaseHz * std::exp2(octaves), voice.q,
            m_sampleRate, voice.filterGain);
      }

      double gainMod = 1.;
      if(p.lfoDest == SamplerParams::LfoAmp)
        gainMod = 1. - p.lfoDepth * 0.5 * (1. + lfoVal); // tremolo, downwards
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

      // Exclusive-class cut: a fast-but-audible fade (FluidSynth forces a
      // ~0.3 s release; a 10 ms chop was found too abrupt on open hi-hats)
      const double chokeStep
          = voice.choked ? 1.0 / std::max(1.0, m_sampleRate * 0.3) : 0.0;

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

      if(voice.playing && voice.amp_adsr.done())
        stop_voice(voice);
    }
  }

private:
  void note_on(GigInstrument& instr, int note, int velocity) noexcept
  {
    const auto& p = m_params;
    m_noteVelocity[note & 127] = (uint8_t)velocity;

    // Chromatic mode: every key plays what is mapped at the root, repitched
    const int matchNote = p.chromatic ? std::clamp(p.chromaticRoot, 0, 127) : note;

    const int regionCount = std::min<int>(instr.regions.size(), max_regions);

    // A key with no mapped region is a no-op: in particular it must not
    // choke the held mono/legato note
    {
      bool anyMatch = false;
      for(int i = 0; i < regionCount && !anyMatch; i++)
      {
        auto& region = instr.regions[i];
        anyMatch = !region.muted && !region.releaseTrigger
                   && regionMatches(region, matchNote, velocity);
      }
      if(!anyMatch)
        return;
    }

    // 1. chokes: any group this hit triggers cuts what currently sounds in it
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
          v.choked = true;
    }

    // 2. mono / legato handling for melodic content
    const bool monoish = p.voiceMode != SamplerParams::Poly;
    bool legatoTransfer = false;
    if(monoish)
    {
      bool anyHeld = false;
      for(auto& v : m_voices)
        if(v.playing && !v.released && !v.choked && v.region && v.region->pitchTrack)
          anyHeld = true;

      if(p.voiceMode == SamplerParams::Legato && anyHeld)
      {
        // Same zone still held: just glide there, no retrigger at all
        bool retargeted = false;
        for(auto& v : m_voices)
        {
          if(v.playing && !v.released && !v.choked && v.region
             && v.region->pitchTrack && matchNote >= v.region->keyLow
             && matchNote <= v.region->keyHigh)
          {
            const double target
                = basePitchSemitones(*v.region, p, matchNote) + (note - matchNote);
            v.glide.start(v.glide.current, target, true);
            v.note = note;
            m_lastPitchSemis = target;
            m_haveLastPitch = true;
            retargeted = true;
          }
        }
        if(retargeted)
          return;
      }

      // Crossing into another zone (or mono retrigger): the previous melodic
      // voices are choked...
      for(auto& v : m_voices)
        if(v.playing && !v.choked && v.region && v.region->pitchTrack)
          v.choked = true;

      // ...and in legato, while a note was still held, the replacement voice
      // continues the phrase: its attack is suppressed so the zone change
      // crossfades instead of re-articulating
      legatoTransfer = p.voiceMode == SamplerParams::Legato && anyHeld;
    }

    // 3. zone matching with round-robin/random alternation. The loader
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
        start_voice(region, note, matchNote, velocity, false, legatoTransfer);
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
        start_voice(region, note, matchNote, velocity, false, legatoTransfer);
    }
  }

  void note_off(GigInstrument& instr, int note) noexcept
  {
    for(auto& voice : m_voices)
    {
      if(!voice.playing || voice.released)
        continue;
      if(voice.note != note)
        continue;
      if(voice.oneShotEff)
        continue;

      voice.amp_adsr.startRelease();
      voice.filterEnv.release();
      voice.released = true;
    }

    // Release triggers: dedicated regions fired on note-off (gig)
    const int velocity = m_noteVelocity[note & 127];
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
      start_voice(region, note, matchNote, velocity, true, false);
    }
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
      const GigRegion& region, int note, int matchNote, int velocity,
      bool fromRelease, bool legatoTransfer = false) noexcept
  {
    if(!region.sample.data || region.sample.data->empty()
       || (*region.sample.data)[0].empty())
      return;
    const int64_t frames = std::ssize((*region.sample.data)[0]);
    const auto& p = m_params;

    auto& voice = allocate_voice();
    voice.region = &region;
    voice.note = note;
    voice.velocity = velocity;
    voice.startOrder = m_voiceCounter++;
    voice.playing = true;
    voice.released = false;
    voice.choked = false;
    voice.chokeGain = 1.0;
    voice.oneShotEff = region.oneShot || fromRelease;

    // Pitch: static part (matched note for keytracking, plus the chromatic
    // distance to the played note) + glide start; live modulation per block
    const double base
        = basePitchSemitones(region, p, matchNote) + (note - matchNote);
    voice.randomSemis = region.randomPitch > 0
                            ? region.randomPitch * (2.0 * random01() - 1.0)
                            : 0.;
    const bool glideOn = p.voiceMode != SamplerParams::Poly && p.glide > 0.f
                         && region.pitchTrack && m_haveLastPitch;
    voice.glide.start(glideOn ? m_lastPitchSemis : base, base, glideOn);
    if(region.pitchTrack)
    {
      m_lastPitchSemis = base;
      m_haveLastPitch = true;
    }
    voice.pitchEnv.value = 0.;
    if(p.pitchEnvAmount != 0.f)
      voice.pitchEnv.trigger(p.pitchEnvAmount);

    // Start position: region offset + global/velocity start offset
    const double startFrac = std::clamp(
        p.startOffset + p.velToStart * (1.0 - velocity / 127.0), 0., 0.95);
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

    // Gain: velocity curve, zone crossfade, region attenuation
    voice.gain = region.sampleAttenuation * velocityGain(region, p, velocity)
                 * velocityZoneGain(region, velocity, p.velXfade);

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
    voice.amp_adsr.amp(1.0);

    // Filter
    voice.filter.reset();
    switch(p.filterType)
    {
      case SamplerParams::FilterFromFile:
        voice.filterOn = region.vcfEnabled;
        voice.filterTypeEff = SamplerParams::FilterLowpass;
        voice.cutoffBaseHz
            = 20.0 * std::pow(1000.0, region.vcfCutoff / 127.0);
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
    voice.lfo = {};
    voice.vibLfo = {};
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
  double m_lastPitchSemis{};
  bool m_haveLastPitch{};
  int64_t m_voiceCounter{};
  uint32_t m_rngState{0x9E3779B9};
  uint8_t m_rrCounter[128]{};
  uint8_t m_noteVelocity[128]{};
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
      else if(i == EnvFromFile) // a bool-valued combo box
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
