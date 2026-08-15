#include "Component.hpp"

#include <Execution/DocumentPlugin.hpp>

#include <score/tools/Bind.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/detail/ssize.hpp>

#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>
#include <Deuterium/GigSampler/SamplerEngine.hpp>
#include <Gamma/Envelope.h>
#include <halp/compat/gamma.hpp>
#include <libremidi/detail/conversion.hpp>

#include <cmath>

namespace Deuterium::Gig
{
namespace Executor
{

struct gig_voice
{
  const GigRegion* region{};
  gam::ADSR<double, double, halp::compat::gamma_domain> amp_adsr;

  Biquad filter;
  LofiState lofi;
  PlayHead head;
  ResolvedLoop loop;
  Lfo lfo;
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
  double chokeGain{1.};
  double gain{1.};       // velocity * zone crossfade * region attenuation
  double randomSemis{};
};

class gigsampler_node final : public ossia::graph_node
{
public:
  static constexpr int max_voices = 64;

  gigsampler_node(ossia::execution_state& st)
      : m_st{st}
  {
    this->m_inlets.push_back(midi_in = new ossia::midi_inlet);
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
    applySamplerControl(m_params, control, v);
  }

  [[nodiscard]] std::string label() const noexcept override { return "gigsampler"; }

  void all_notes_off() noexcept override
  {
    for(auto& v : m_voices)
    {
      if(v.playing && !v.released)
      {
        v.amp_adsr.release();
        v.filterEnv.release();
        v.released = true;
      }
    }
  }

  void run(const ossia::token_request& tk, ossia::exec_state_facade estate) noexcept override
  {
    if(!m_gigInfo)
      return;
    if(m_gigInfo->selectedInstrument < 0
       || m_gigInfo->selectedInstrument >= std::ssize(m_gigInfo->instruments))
      return;

    this->audio_out->data.set_channels(2);
    for(int i = 0; i < 2; i++)
    {
      this->audio_out->data.get()[i].clear();
      this->audio_out->data.get()[i].resize(estate.bufferSize(), 0.);
    }

    double* outs[2] = {
        this->audio_out->data.channel(0).data(),
        this->audio_out->data.channel(1).data()};

    auto& instr = m_gigInfo->instruments[m_gigInfo->selectedInstrument];

    for(auto& mess : this->midi_in->data.messages)
    {
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

    const auto timings = estate.timings(tk);
    double* out_l = outs[0] + timings.start_sample;
    double* out_r = outs[1] + timings.start_sample;

    const auto& p = m_params;
    const int64_t xfadeFrames = (int64_t)(p.loopXfade * m_sampleRate);

    for(auto& voice : m_voices)
    {
      if(!voice.playing || !voice.region)
        continue;

      auto& region = *voice.region;
      auto& sampleData = region.sample.data;
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
            m_sampleRate);
      }

      double gainMod = 1.;
      if(p.lfoDest == SamplerParams::LfoAmp)
        gainMod = 1. - p.lfoDepth * 0.5 * (1. + lfoVal); // tremolo, downwards
      double panMod = 0.;
      if(p.lfoDest == SamplerParams::LfoPan)
        panMod = lfoVal * p.lfoDepth;

      const double pan
          = std::clamp(region.pan / 64.0 + p.pan + panMod, -1., 1.);
      const double panR = (pan + 1.) * 0.5;
      const double panL = 1. - panR;
      const double gainTotal = voice.gain * p.volume * gainMod;

      // Anti-click fadeout near the end of non-looping playback
      const int64_t fadeSamples = (int64_t)(m_sampleRate * 0.00075);

      const double chokeStep
          = voice.choked ? 1.0 / std::max(1.0, m_sampleRate * 0.010) : 0.0;

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

    const int regionCount = std::min<int>(instr.regions.size(), 512);

    // 1. chokes: any group this hit triggers cuts what currently sounds in it
    for(int i = 0; i < regionCount; i++)
    {
      auto& region = instr.regions[i];
      if(region.muted || region.releaseTrigger || region.chokeGroup < 0)
        continue;
      if(!regionMatches(region, note, velocity))
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
             && v.region->pitchTrack && note >= v.region->keyLow
             && note <= v.region->keyHigh)
          {
            const double target = basePitchSemitones(*v.region, p, note);
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

    // 3. zone matching with round-robin/random alternation.
    // Alternatives share the exact same key and velocity zone.
    bool used[512]{};
    for(int i = 0; i < regionCount; i++)
    {
      if(used[i])
        continue;
      auto& region = instr.regions[i];
      if(region.muted || region.releaseTrigger)
        continue;
      if(!regionMatches(region, note, velocity))
        continue;

      int group[32];
      int groupSize = 0;
      for(int j = i; j < regionCount && groupSize < 32; j++)
      {
        if(used[j])
          continue;
        auto& other = instr.regions[j];
        if(other.muted || other.releaseTrigger)
          continue;
        if(other.keyLow == region.keyLow && other.keyHigh == region.keyHigh
           && other.velLow == region.velLow && other.velHigh == region.velHigh)
        {
          used[j] = true;
          group[groupSize++] = j;
        }
      }

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
          policy = groupSize > 1 ? SamplerParams::RRCycle : 0;
          break;
        case SamplerParams::RRRandom:
          policy = groupSize > 1 ? SamplerParams::RRRandom : 0;
          break;
      }

      if(policy == 0)
      {
        for(int g = 0; g < groupSize; g++)
          start_voice(instr.regions[group[g]], note, velocity, false, legatoTransfer);
      }
      else
      {
        const int pick
            = pickAlternative(groupSize, policy, m_rrCounter[note & 127], m_rngState);
        start_voice(instr.regions[group[pick]], note, velocity, false, legatoTransfer);
      }
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

      voice.amp_adsr.release();
      voice.filterEnv.release();
      voice.released = true;
    }

    // Release triggers: dedicated regions fired on note-off (gig)
    const int velocity = m_noteVelocity[note & 127];
    for(auto& region : instr.regions)
    {
      if(!region.releaseTrigger || region.muted)
        continue;
      if(!regionMatches(region, note, velocity))
        continue;
      start_voice(region, note, velocity, true, false);
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

    // At the cap (or full pool): steal the oldest voice
    gig_voice* oldest = &m_voices[0];
    for(auto& v : m_voices)
      if(v.playing && v.startOrder < oldest->startOrder)
        oldest = &v;
    return *oldest;
  }

  void start_voice(
      const GigRegion& region, int note, int velocity, bool fromRelease,
      bool legatoTransfer = false) noexcept
  {
    auto& sampleData = region.sample.data;
    if(sampleData.empty() || sampleData[0].empty())
      return;
    const int64_t frames = std::ssize(sampleData[0]);
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

    // Pitch: static part + glide start; live modulation comes per block
    const double base = basePitchSemitones(region, p, note);
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
    voice.amp_adsr.decay(std::max(0.001, env.decay));
    voice.amp_adsr.sustain(env.sustain);
    voice.amp_adsr.release(std::max(0.005, env.release));
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
        voice.q = 1.0 + region.vcfResonance * 9.0 / 127.0;
        break;
      case SamplerParams::FilterOff:
        voice.filterOn = false;
        break;
      default:
        voice.filterOn = true;
        voice.filterTypeEff = p.filterType;
        voice.cutoffBaseHz = p.cutoff;
        voice.q = 0.5 + p.resonance * 9.5;
        break;
    }
    if(voice.filterOn)
      voice.filter.configure(
          voice.filterTypeEff, voice.cutoffBaseHz, voice.q, m_sampleRate);

    voice.filterEnv = {};
    voice.filterEnv.trigger();
    voice.lfo = {};
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
  ossia::audio_outlet* audio_out{};
  std::vector<gig_voice> m_voices;
  SamplerParams m_params;
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

  // Control inlets: initial values now, updates through the command queue
  const auto& inlets = proc.inlets();
  for(int i = 0; i < ControlCount && 1 + i < std::ssize(inlets); i++)
  {
    auto* ctl = qobject_cast<Process::ControlInlet*>(inlets[1 + i]);
    if(!ctl)
      continue;
    node->set_control(i, ctl->value());
    connect(
        ctl, &Process::ControlInlet::valueChanged, this,
        [this, node, i](const ossia::value& v) {
      in_exec([node, i, v] { node->set_control(i, v); });
    });
  }

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
