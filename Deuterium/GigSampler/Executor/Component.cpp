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

  int64_t position{};
  bool playing{};
  bool released{};
  double pitchRatio{1.0};
};

class gigsampler_node final : public ossia::graph_node
{
public:
  gigsampler_node(ossia::execution_state& st)
      : m_st{st}
  {
    this->m_inlets.push_back(midi_in = new ossia::midi_inlet);
    this->m_outlets.push_back(audio_out = new ossia::audio_outlet);
  }

  void reload(
      std::shared_ptr<GigFileInfo>&& info,
      std::vector<gig_voice>&& v,
      double rate)
  {
    using namespace std;
    swap(info, m_gigInfo);
    swap(v, m_voices);
    m_sampleRate = rate;
  }

  [[nodiscard]] std::string label() const noexcept override { return "gigsampler"; }

  void all_notes_off() noexcept override
  {
    for(auto& v : m_voices)
    {
      if(v.playing)
      {
        v.amp_adsr.release();
        v.released = true;
      }
    }
  }

  void run(const ossia::token_request& tk, ossia::exec_state_facade estate) noexcept override
  {
    if(!m_gigInfo || m_voices.empty())
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

          // Find all regions that match this note and velocity
          {
            int matched = 0;
            for(auto& voice : m_voices)
            {
              if(!voice.region)
                continue;
              if(note >= voice.region->keyLow && note <= voice.region->keyHigh
                 && velocity >= voice.region->velLow && velocity <= voice.region->velHigh)
                matched++;
            }
            if(matched == 0)
              qWarning() << "GigSampler: NOTE_ON" << note << "vel" << velocity
                         << "-> NO MATCH in" << m_voices.size() << "voices";
            else
              qWarning() << "GigSampler: NOTE_ON" << note << "vel" << velocity
                         << "-> matched" << matched << "voices";
          }
          for(auto& voice : m_voices)
          {
            if(!voice.region)
              continue;
            if(note < voice.region->keyLow || note > voice.region->keyHigh)
              continue;
            if(velocity < voice.region->velLow || velocity > voice.region->velHigh)
              continue;

            voice.playing = true;
            voice.released = false;
            voice.position = voice.region->sampleStartOffset;

            // Compute pitch ratio for pitch tracking
            if(voice.region->pitchTrack)
            {
              int semitoneDiff = note - (int)voice.region->sample.midiUnityNote;
              voice.pitchRatio = std::pow(2.0, semitoneDiff / 12.0);
            }
            else
            {
              voice.pitchRatio = 1.0;
            }

            // Velocity-scaled attenuation
            double velGain = velocity / 127.0;

            voice.amp_adsr.reset();
            voice.amp_adsr.set_sample_rate(m_sampleRate);
            voice.amp_adsr.attack(std::max(0.75, voice.region->eg1Attack * 1000.0));
            voice.amp_adsr.decay(voice.region->eg1Decay * 1000.0);
            voice.amp_adsr.sustain(voice.region->eg1Sustain);
            voice.amp_adsr.release(std::max(0.75, voice.region->eg1Release * 1000.0));
            voice.amp_adsr.amp(voice.region->sampleAttenuation * velGain);
          }
          break;
        }
        note_off:
        case libremidi::message_type::NOTE_OFF: {
          for(auto& voice : m_voices)
          {
            if(!voice.playing || voice.released)
              continue;
            if(!voice.region)
              continue;
            if(note < voice.region->keyLow || note > voice.region->keyHigh)
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
      if(!voice.playing)
        continue;

      auto& region = *voice.region;
      auto& sampleData = region.sample.data;
      const int channels = sampleData.size();
      if(channels == 0)
        continue;

      const int64_t totalFrames = std::ssize(sampleData[0]);
      const double pitchRatio = voice.pitchRatio;

      // Anti-click fadeout near sample end (0.5ms)
      const int64_t fadeSamples = (int64_t)(m_sampleRate * 0.00075);
      const int64_t fadeStart = totalFrames - fadeSamples;

      // Pan: -64..63 -> left/right gain
      double panR = (region.pan + 64.0) / 127.0;
      double panL = 1.0 - panR;

      if(region.vcfEnabled)
      {
        for(int64_t k = 0; k < timings.length; k++)
        {
          double srcPos = voice.position + k * pitchRatio;
          int64_t idx = (int64_t)srcPos;
          double frac = srcPos - idx;

          if(idx >= totalFrames)
          {
            if(region.sample.hasLoop && region.sample.loopEnd > region.sample.loopStart)
            {
              int64_t loopLen = region.sample.loopEnd - region.sample.loopStart;
              idx = region.sample.loopStart + ((idx - region.sample.loopStart) % loopLen);
            }
            else
            {
              voice.playing = false;
              break;
            }
          }

          double sL, sR;
          if(channels == 1)
          {
            double s0 = sampleData[0][idx];
            double s1 = (idx + 1 < totalFrames) ? sampleData[0][idx + 1] : s0;
            double s = s0 + (s1 - s0) * frac;
            sL = s;
            sR = s;
          }
          else
          {
            double s0l = sampleData[0][idx];
            double s1l = (idx + 1 < totalFrames) ? sampleData[0][idx + 1] : s0l;
            double s0r = sampleData[1][idx];
            double s1r = (idx + 1 < totalFrames) ? sampleData[1][idx + 1] : s0r;
            sL = s0l + (s1l - s0l) * frac;
            sR = s0r + (s1r - s0r) * frac;
          }

          // Anti-click fadeout near sample end
          if(!region.sample.hasLoop && idx >= fadeStart && fadeSamples > 0)
          {
            double fadeGain = (double)(totalFrames - idx) / (double)fadeSamples;
            sL *= fadeGain;
            sR *= fadeGain;
          }

          double arr[2] = {sL, sR};
          double* parr[2] = {&arr[0], &arr[1]};
          voice.lowpassFilter.process(1, parr);

          double aenv = voice.amp_adsr();
          out_l[k] += arr[0] * aenv * panL;
          out_r[k] += arr[1] * aenv * panR;
        }
      }
      else
      {
        for(int64_t k = 0; k < timings.length; k++)
        {
          double srcPos = voice.position + k * pitchRatio;
          int64_t idx = (int64_t)srcPos;
          double frac = srcPos - idx;

          if(idx >= totalFrames)
          {
            if(region.sample.hasLoop && region.sample.loopEnd > region.sample.loopStart)
            {
              int64_t loopLen = region.sample.loopEnd - region.sample.loopStart;
              idx = region.sample.loopStart + ((idx - region.sample.loopStart) % loopLen);
            }
            else
            {
              voice.playing = false;
              break;
            }
          }

          double sL, sR;
          if(channels == 1)
          {
            double s0 = sampleData[0][idx];
            double s1 = (idx + 1 < totalFrames) ? sampleData[0][idx + 1] : s0;
            double s = s0 + (s1 - s0) * frac;
            sL = s;
            sR = s;
          }
          else
          {
            double s0l = sampleData[0][idx];
            double s1l = (idx + 1 < totalFrames) ? sampleData[0][idx + 1] : s0l;
            double s0r = sampleData[1][idx];
            double s1r = (idx + 1 < totalFrames) ? sampleData[1][idx + 1] : s0r;
            sL = s0l + (s1l - s0l) * frac;
            sR = s0r + (s1r - s0r) * frac;
          }

          // Anti-click fadeout near sample end
          if(!region.sample.hasLoop && idx >= fadeStart && fadeSamples > 0)
          {
            double fadeGain = (double)(totalFrames - idx) / (double)fadeSamples;
            sL *= fadeGain;
            sR *= fadeGain;
          }

          double aenv = voice.amp_adsr();
          out_l[k] += sL * aenv * panL;
          out_r[k] += sR * aenv * panR;
        }
      }

      voice.position += (int64_t)(timings.length * pitchRatio);

      // Check if voice finished
      if(voice.amp_adsr.done())
      {
        voice.playing = false;
        voice.position = 0;
        voice.released = false;
      }
    }
  }

  ossia::execution_state& m_st;
  std::shared_ptr<GigFileInfo> m_gigInfo;
  ossia::midi_inlet* midi_in{};
  ossia::audio_outlet* audio_out{};
  std::vector<gig_voice> m_voices;
  double m_sampleRate{48000.0};
};

static std::vector<gig_voice>
loadVoices(const GigFileInfo& info, double rate)
{
  std::vector<gig_voice> voices;

  if(info.selectedInstrument < 0
     || info.selectedInstrument >= (int)info.instruments.size())
    return voices;

  auto& instr = info.instruments[info.selectedInstrument];
  voices.reserve(instr.regions.size());

  for(auto& region : instr.regions)
  {
    gig_voice v;
    v.region = &region;

    v.amp_adsr.reset();
    v.amp_adsr.set_sample_rate(rate);
    v.amp_adsr.attack(std::max(0.75, region.eg1Attack * 1000.0));
    v.amp_adsr.decay(region.eg1Decay * 1000.0);
    v.amp_adsr.sustain(region.eg1Sustain);
    v.amp_adsr.release(std::max(0.75, region.eg1Release * 1000.0));

    // Setup filter
    Dsp::Params params;
    params[0] = rate;
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
    v.lowpassFilter.setParams(params);

    voices.push_back(std::move(v));
  }

  return voices;
}

Component::Component(
    Deuterium::Gig::ProcessModel& proc, const ::Execution::Context& ctx,
    QObject* parent)
    : ::Execution::ProcessComponent_T<Deuterium::Gig::ProcessModel, ossia::node_process>{
        proc, ctx, "GigSamplerComponent", parent}
{
  std::shared_ptr<gigsampler_node> node
      = ossia::make_node<gigsampler_node>(*ctx.execState, *ctx.execState);

  if(proc.gigInfo())
  {
    auto gi = proc.gigInfo();
    auto v = loadVoices(*gi, ctx.execState->sampleRate);
    qWarning() << "GigSampler: constructor load -> voices:" << v.size()
               << "regions:" << gi->instruments[gi->selectedInstrument].regions.size();
    if(!v.empty())
    {
      auto& r = *v[0].region;
      qWarning() << "  first voice: key" << r.keyLow << "-" << r.keyHigh
                 << "vel" << r.velLow << "-" << r.velHigh
                 << "sampleCh:" << r.sample.data.size()
                 << "frames:" << (r.sample.data.empty() ? 0 : (int)r.sample.data[0].size());
    }
    node->reload(std::move(gi), std::move(v), ctx.execState->sampleRate);
  }
  else
  {
    qWarning() << "GigSampler: constructor -> gigInfo is null, waiting for async load";
  }
  this->node = node;

  m_ossia_process = std::make_shared<ossia::node_process>(node);

  connect(&proc, &Deuterium::Gig::ProcessModel::fileChanged, this, [this, node] {
    if(auto gi = this->process().gigInfo())
    {
      auto rate = this->system().execState->sampleRate;
      auto v = loadVoices(*gi, rate);
      qWarning() << "GigSampler: executor fileChanged -> voices:" << v.size();
      in_exec([node, gi, v = std::move(v), rate]() mutable {
        node->reload(std::move(gi), std::move(v), rate);
      });
    }
    else
    {
      qWarning() << "GigSampler: executor fileChanged but gigInfo is null!";
    }
  });
}

Component::~Component() { }

}
}
