// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "Component.hpp"

#include <Explorer/DocumentPlugin/DeviceDocumentPlugin.hpp>

#include <Scenario/Execution/score2OSSIA.hpp>

#include <Execution/DocumentPlugin.hpp>

#include <score/serialization/AnySerialization.hpp>
#include <score/serialization/MapSerialization.hpp>
#include <score/tools/Bind.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/graph/graph_interface.hpp>
#include <ossia/dataflow/graph_edge.hpp>
#include <ossia/dataflow/graph_edge_helpers.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/detail/logger.hpp>
#include <ossia/detail/ssize.hpp>
#include <ossia/editor/scenario/time_interval.hpp>
#include <ossia/editor/state/message.hpp>
#include <ossia/editor/state/state.hpp>

#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlContext>
#include <QTimer>

#include <Deuterium/ProcessModel.hpp>
#include <DspFilters/Filter.h>
#include <DspFilters/RBJ.h>
#include <DspFilters/SmoothedFilter.h>
#include <Gamma/Envelope.h>
#include <halp/compat/gamma.hpp>
#include <libremidi/detail/conversion.hpp>
namespace Deuterium
{
namespace Executor
{
struct drum_layer
{
  Layer* layer{};
  gam::ADSR<double, double, halp::compat::gamma_domain> amp_adsr;

  static constexpr auto chans = 2;
  Dsp::SmoothedFilterDesign<Dsp::RBJ::Design::LowPass, chans> lowpassFilter{128};
};

struct drum_voice
{
  Instrument* instrument{};
  std::vector<drum_layer> layers;

  int64_t position{};
  bool playing{};
};

class deuterium_node final : public ossia::graph_node
{
public:
  deuterium_node(std::shared_ptr<DrumkitInfo> kit, ossia::execution_state& st);

  void run(const ossia::token_request& t, ossia::exec_state_facade) noexcept override;
  [[nodiscard]] std::string label() const noexcept override { return "deuterium"; }

  void all_notes_off() noexcept override;

  ossia::execution_state& m_st;

  std::shared_ptr<DrumkitInfo> m_drumkit;
  ossia::midi_inlet* midi_in{};
  ossia::audio_outlet* audio_out{};

  ossia::flat_map<int, drum_voice> voices;
};

Component::Component(
    Deuterium::ProcessModel& proc, const ::Execution::Context& ctx, QObject* parent)
    : ::Execution::ProcessComponent_T<Deuterium::ProcessModel, ossia::node_process>{
        proc, ctx, "DeuteriumComponent", parent}
{
  std::shared_ptr<deuterium_node> node
      = ossia::make_node<deuterium_node>(*ctx.execState, proc.drumkit(), *ctx.execState);

  this->node = node;
  m_ossia_process = std::make_shared<ossia::node_process>(node);
}

Component::~Component() { }

deuterium_node::deuterium_node(
    std::shared_ptr<DrumkitInfo> kit, ossia::execution_state& st)
    : m_st{st}
{
  this->m_inlets.push_back(midi_in = new ossia::midi_inlet);
  this->m_outlets.push_back(audio_out = new ossia::audio_outlet);

  voices.reserve(kit->instruments.size());
  for(auto& inst : kit->instruments)
  {
    drum_voice v;
    v.instrument = &inst;
    v.layers.reserve(inst.layers.size());
    for(auto& lay : inst.layers)
    {
      drum_layer& l = v.layers.emplace_back();
      l.layer = &lay;

      l.amp_adsr.reset();
      l.amp_adsr.set_sample_rate(48000.);
      l.amp_adsr.attack(inst.Attack);
      l.amp_adsr.decay(inst.Decay);
      l.amp_adsr.sustain(inst.Sustain);
      l.amp_adsr.release(inst.Release);

      Dsp::Params params;
      params[0] = 48000;
      if(inst.filterActive)
      {
        params[1] = std::clamp(inst.filterCutoff, 20., 20000.);
        params[2] = inst.filterResonance;
      }
      else
      {
        params[1] = 20000.;
        params[2] = 1.;
      }
      l.lowpassFilter.setParams(params);
    }

    if(!v.layers.empty())
    {
      voices[inst.midi_note] = std::move(v);
    }
  }
}

void deuterium_node::run(
    const ossia::token_request& tk, ossia::exec_state_facade estate) noexcept
{
  // Setup audio output
  double** outs{};
  int out_count{};
  {
    out_count = 2;
    outs = (double**)alloca(sizeof(double*) * out_count);
    this->audio_out->data.set_channels(2);
    for(int i = 0; i < out_count; i++)
    {
      // FIXME do it properly with token request
      this->audio_out->data.get()[i].clear();
      this->audio_out->data.get()[i].resize(estate.bufferSize(), 0.);
      outs[i] = this->audio_out->data.channel(i).data();
    }
  }

  // Setup midi
  for(auto& mess : this->midi_in->data.messages)
  {
    uint8_t data[4];
    int n = cmidi2_convert_single_ump_to_midi1((uint8_t*)data, 3, mess.data);
    if(n == 3)
    {
      libremidi::message m{{data[0], data[1], data[2]}};

      const int note = data[1];
      switch(m.get_message_type())
      {
        case libremidi::message_type::NOTE_ON: {
          auto it = this->voices.find(note);
          if(it == this->voices.end())
            continue;
          it->second.playing = true;
          it->second.position = 0;

          auto& inst = *it->second.instrument;
          for(auto& l : it->second.layers)
          {
            l.amp_adsr.reset();
            l.amp_adsr.set_sample_rate(48000.);
            l.amp_adsr.attack(inst.Attack);
            l.amp_adsr.decay(inst.Decay);
            l.amp_adsr.sustain(inst.Sustain);
            l.amp_adsr.release(inst.Release);
            l.amp_adsr.amp(inst.volume);
          }

          break;
        }
        case libremidi::message_type::NOTE_OFF: {
          auto it = this->voices.find(note);
          if(it == this->voices.end())
            continue;
          if(it->second.playing)
          {
            for(auto& l : it->second.layers)
              l.amp_adsr.release();
          }
          break;
        }
        default:
          break;
      }
    }
  }

  const auto timings = estate.timings(tk);
  double* out_l = outs[0] + timings.start_sample;
  double* out_r = outs[1] + timings.start_sample;

  for(auto& voice : this->voices)
  {
    if(!voice.second.playing)
      continue;

    for(auto& lay : voice.second.layers)
    {
      auto& data = lay.layer->data;
      const int channels = data.size();
      const int64_t frames = std::ssize(data[0]);
      const int64_t pos = voice.second.position;
      const auto n = std::min((int64_t)(pos + timings.length), frames);

      if(channels == 1)
      {
        for(int64_t i = pos, k = 0; i < n; i++, k++)
        {
          double sample = data[0][i];
          double arr[2] = {sample, sample};
          double* parr[2] = {&arr[0], &arr[1]};
          lay.lowpassFilter.process(1, parr);

          double aenv = lay.amp_adsr();
          out_l[k] += arr[0] * aenv;
          out_r[k] += arr[1] * aenv;
        }
      }
      else if(channels == 2)
      {
        for(int64_t i = pos, k = 0; i < n; i++, k++)
        {
          double arr[2] = {data[0][i], data[1][i]};
          double* parr[2] = {&arr[0], &arr[1]};
          lay.lowpassFilter.process(1, parr);

          double aenv = lay.amp_adsr();
          out_l[k] += arr[0] * aenv;
          out_r[k] += arr[1] * aenv;
        }
      }
    }
    voice.second.position += timings.length;
    int finished{0};
    for(auto& lay : voice.second.layers)
    {
      if(std::ssize(lay.layer->data[0]) <= voice.second.position || lay.amp_adsr.done())
        finished++;
    }
    if(finished == std::ssize(voice.second.layers))
    {
      voice.second.playing = false;
      voice.second.position = 0;
      for(auto& lay : voice.second.layers)
      {
        lay.amp_adsr.release();
      }
    }
  }
}

void deuterium_node::all_notes_off() noexcept { }
}
}
