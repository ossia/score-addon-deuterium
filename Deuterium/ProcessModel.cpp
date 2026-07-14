// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "ProcessModel.hpp"

#include <Process/Dataflow/Port.hpp>

#include <Deuterium/ApplicationPlugin.hpp>
#include <Deuterium/Drumkit.hpp>
#include <Deuterium/Library.hpp>
#include <Deuterium/ProcessMetadata.hpp>

#include <wobjectimpl.h>
W_OBJECT_IMPL(Deuterium::ProcessModel)

namespace Deuterium
{

ProcessModel::ProcessModel(
    const TimeVal& duration, const QString& data, const Id<Process::ProcessModel>& id,
    QObject* parent)
    : Process::
          ProcessModel{duration, id, Metadata<ObjectKey_k, ProcessModel>::get(), parent}
    , midi_in{std::make_unique<Process::MidiInlet>(
          "MIDI In", Id<Process::Port>(0), this)}
    , audio_out{std::make_unique<Process::AudioOutlet>(
          "Audio Out", Id<Process::Port>(0), this)}
    , m_drumkitPath{data}
{
  metadata().setInstanceName(*this);

  // Only attempt to load a drumkit when we were handed a Hydrogen file. Empty
  // or non-Hydrogen data yields a valid, silent process (no drumkit loaded)
  // rather than failing construction.
  if(data.contains("drumkit.xml"))
    m_drumkit = parseDrumkit(data);

  m_inlets.push_back(midi_in.get());
  m_outlets.push_back(audio_out.get());
  ((Process::AudioOutlet*)audio_out.get())->setPropagate(true);
}

ProcessModel::~ProcessModel() { }

QString ProcessModel::effect() const noexcept
{
  return m_drumkitPath;
}

void ProcessModel::loadDrumkit(const QString& path)
{
  auto drumkit = parseDrumkit(path);
  if(drumkit)
  {
    m_drumkit = drumkit;
    m_drumkitPath = path;
    drumkitChanged();
  }
}

std::vector<Process::Preset> ProcessModel::builtinPresets() const noexcept
{
  static auto library_presets = [] {
    std::vector<Process::Preset> presets;
    const auto* libs
        = score::GUIAppContext().interfaces<Library::LibraryInterfaceList>().get(
            LibraryHandler::static_concreteKey());
    if(!libs)
      return presets;

    auto node = static_cast<const LibraryHandler*>(libs)->node;
    for(auto& cld : node->children())
    {
      Process::Preset p;
      p.name = cld.prettyName;
      p.key = {cld.key, cld.customData};
      presets.push_back(p);
    }

    return presets;
  }();
  return library_presets;
}

void ProcessModel::loadPreset(const Process::Preset& preset)
{
  loadDrumkit(preset.key.effect);
}
}
