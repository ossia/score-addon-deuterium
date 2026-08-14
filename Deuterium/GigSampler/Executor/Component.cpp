#include "Component.hpp"

#include <Explorer/DocumentPlugin/DeviceDocumentPlugin.hpp>

#include <Scenario/Execution/score2OSSIA.hpp>

#include <Execution/DocumentPlugin.hpp>

#include <score/tools/Bind.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/detail/ssize.hpp>

#include <Deuterium/GigSampler/ProcessModel.hpp>
#include <DspFilters/Filter.h>
#include <DspFilters/RBJ.h>
#include <DspFilters/SmoothedFilter.h>
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

  static constexpr auto chans = 2;
  Dsp::SmoothedFilterDesign<Dsp::RBJ::Design::LowPass, chans> lowpassFilter{128};

  int note{-1};
  int64_t startOrder{};
  // Fractional playback position: integer truncation of the per-block
  // advance would detune pitched voices and jump backwards at block edges
  double position{};
  bool playing{};
  bool released{};
  bool choked{};
  double chokeGain{1.0};
  double pitchRatio{1.0};
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
  }

  [[nodiscard]] std::string label() const noexcept override { return "gigsampler"; }

  void all_notes_off() noexcept override
  {
    for(auto& v : m_voices)
    {
      if(v.playing && !v.released)
      {
        v.amp_adsr.release();
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

    // Setup audio output
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

    // Process MIDI input
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

          // Choke pass first, so that voices started by this same hit are
          // never choked by their sibling layers: any group this hit triggers
          // cuts all currently sounding voices of the group — including
          // previous instances of the same region (hi-hat self-choke)
          for(auto& region : instr.regions)
          {
            if(region.muted || region.chokeGroup < 0)
              continue;
            if(note < region.keyLow || note > region.keyHigh)
              continue;
            if(velocity < region.velLow || velocity > region.velHigh)
              continue;

            for(auto& v : m_voices)
              if(v.playing && !v.choked && v.region
                 && v.region->chokeGroup == region.chokeGroup)
                v.choked = true;
          }

          // Start a voice for every region matching this note and velocity
          for(auto& region : instr.regions)
          {
            if(region.muted)
              continue;
            if(note < region.keyLow || note > region.keyHigh)
              continue;
            if(velocity < region.velLow || velocity > region.velHigh)
              continue;

            start_voice(allocate_voice(), region, note, velocity);
          }
          break;
        }
        note_off:
        case libremidi::message_type::NOTE_OFF: {
          for(auto& voice : m_voices)
          {
            if(!voice.playing || voice.released)
              continue;
            if(voice.note != note)
              continue;
            // One-shot (drum) voices ignore note-off and play out
            if(voice.region && voice.region->oneShot)
              continue;

            voice.amp_adsr.release();
            voice.released = true;
          }
          break;
        }
        default:
          break;
      }
    }

    const auto timings = estate.timings(tk);
    double* out_l = outs[0] + timings.start_sample;
    double* out_r = outs[1] + timings.start_sample;

    // Render all active voices
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
      const double pitchRatio = voice.pitchRatio;

      const bool hasLoop = region.sample.hasLoop
                           && region.sample.loopEnd > region.sample.loopStart
                           && (int64_t)region.sample.loopEnd <= totalFrames;
      const int64_t loopStart = region.sample.loopStart;
      const int64_t loopEnd = region.sample.loopEnd;
      const int64_t loopLen = loopEnd - loopStart;

      // Anti-click fadeout near sample end (0.75ms)
      const int64_t fadeSamples = (int64_t)(m_sampleRate * 0.00075);
      const int64_t fadeStart = totalFrames - fadeSamples;

      // Pan: -64..63 -> left/right gain
      double panR = (region.pan + 64.0) / 127.0;
      double panL = 1.0 - panR;

      // Choked voices fade out over ~10ms instead of their release stage
      const double chokeStep
          = voice.choked ? 1.0 / std::max(1.0, m_sampleRate * 0.010) : 0.0;

      for(int64_t k = 0; k < timings.length; k++)
      {
        double srcPos = voice.position + k * pitchRatio;
        int64_t idx = (int64_t)srcPos;
        double frac = srcPos - idx;

        if(hasLoop && idx >= loopEnd)
          idx = loopStart + ((idx - loopStart) % loopLen);

        if(idx >= totalFrames || idx < 0)
        {
          stop_voice(voice);
          break;
        }

        double sLR[2];
        if(channels == 1)
        {
          double s0 = sampleData[0][idx];
          double s1 = (idx + 1 < totalFrames) ? sampleData[0][idx + 1] : s0;
          double s = s0 + (s1 - s0) * frac;
          sLR[0] = s;
          sLR[1] = s;
        }
        else
        {
          double s0l = sampleData[0][idx];
          double s1l = (idx + 1 < totalFrames) ? sampleData[0][idx + 1] : s0l;
          double s0r = sampleData[1][idx];
          double s1r = (idx + 1 < totalFrames) ? sampleData[1][idx + 1] : s0r;
          sLR[0] = s0l + (s1l - s0l) * frac;
          sLR[1] = s0r + (s1r - s0r) * frac;
        }

        // Anti-click fadeout near sample end
        if(!hasLoop && idx >= fadeStart && fadeSamples > 0)
        {
          double fadeGain = (double)(totalFrames - idx) / (double)fadeSamples;
          sLR[0] *= fadeGain;
          sLR[1] *= fadeGain;
        }

        if(region.vcfEnabled)
        {
          double* parr[2] = {&sLR[0], &sLR[1]};
          voice.lowpassFilter.process(1, parr);
        }

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
        out_l[k] += sLR[0] * aenv * panL;
        out_r[k] += sLR[1] * aenv * panR;
      }

      if(!voice.playing)
        continue;

      voice.position += timings.length * pitchRatio;

      // Keep the play position bounded while looping
      if(hasLoop && voice.position >= (double)loopEnd)
        voice.position
            = loopStart + std::fmod(voice.position - loopStart, (double)loopLen);

      // Check if voice finished
      if(voice.amp_adsr.done())
        stop_voice(voice);
    }
  }

private:
  gig_voice& allocate_voice() noexcept
  {
    for(auto& v : m_voices)
      if(!v.playing)
        return v;

    // All voices busy: steal the oldest one
    gig_voice* oldest = &m_voices[0];
    for(auto& v : m_voices)
      if(v.startOrder < oldest->startOrder)
        oldest = &v;
    return *oldest;
  }

  void start_voice(
      gig_voice& voice, const GigRegion& region, int note, int velocity) noexcept
  {
    voice.region = &region;
    voice.note = note;
    voice.startOrder = m_voiceCounter++;
    voice.playing = true;
    voice.released = false;
    voice.choked = false;
    voice.chokeGain = 1.0;
    voice.position = region.sampleStartOffset;

    // Compute pitch ratio: key tracking + fine tune + constant offset
    // + per-hit random
    double semitones = region.pitchOffset + region.sample.fineTune / 100.0;
    if(region.pitchTrack)
      semitones += note - (int)region.sample.midiUnityNote;
    if(region.randomPitch > 0)
      semitones += region.randomPitch * (2.0 * random01() - 1.0);
    voice.pitchRatio = std::pow(2.0, semitones / 12.0);

    // Velocity-scaled attenuation
    const double velGain = region.applyVelocity ? velocity / 127.0 : 1.0;

    // libgig reports envelope stage lengths in seconds, which is also what
    // gam::ADSR expects; only enforce a small minimum to avoid clicks.
    voice.amp_adsr.reset();
    voice.amp_adsr.set_sample_rate(m_sampleRate);
    voice.amp_adsr.attack(std::max(0.001, region.eg1Attack));
    voice.amp_adsr.decay(std::max(0.001, region.eg1Decay));
    voice.amp_adsr.sustain(region.eg1Sustain);
    voice.amp_adsr.release(std::max(0.005, region.eg1Release));
    voice.amp_adsr.amp(region.sampleAttenuation * velGain);

    Dsp::Params params;
    params[0] = m_sampleRate;
    if(region.vcfEnabled)
    {
      // VCF cutoff is 0-127, map to frequency
      double freq = 20.0 * std::pow(1000.0, region.vcfCutoff / 127.0);
      freq = std::clamp(freq, 20.0, 20000.0);
      double q = 1.0 + region.vcfResonance * 9.0 / 127.0;
      params[1] = freq;
      params[2] = q;
    }
    else
    {
      params[1] = 20000.;
      params[2] = 1.;
    }
    voice.lowpassFilter.setParams(params);
    voice.lowpassFilter.reset();
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
    voice.position = 0;
    voice.playing = false;
    voice.released = false;
    voice.choked = false;
    voice.chokeGain = 1.0;
  }

public:
  ossia::execution_state& m_st;
  std::shared_ptr<GigFileInfo> m_gigInfo;
  ossia::midi_inlet* midi_in{};
  ossia::audio_outlet* audio_out{};
  std::vector<gig_voice> m_voices;
  double m_sampleRate{48000.0};
  int64_t m_voiceCounter{};
  uint32_t m_rngState{0x9E3779B9};
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
