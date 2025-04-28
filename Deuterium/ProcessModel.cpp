// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "ProcessModel.hpp"

#include <Process/Dataflow/Port.hpp>

#include <Deuterium/ApplicationPlugin.hpp>
#include <Deuterium/Drumkit.hpp>
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
    , midi_in{Process::make_midi_inlet(Id<Process::Port>(0), this)}
    , audio_out{Process::make_audio_outlet(Id<Process::Port>(0), this)}
{
  metadata().setInstanceName(*this);
  qDebug() << data;
  if(!data.contains("drumkit.xml"))
    throw std::runtime_error("Not an Hydrogen file");

  m_drumkit = parseDrumkit(data);
  if(!m_drumkit)
    throw std::runtime_error("Bad Hydrogen file");

  m_inlets.push_back(midi_in.get());
  m_outlets.push_back(audio_out.get());
}

ProcessModel::~ProcessModel() { }

void ProcessModel::loadPreset(const Process::Preset& preset) { }

}
