#include "ProcessModel.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>

#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>

#include <QIODevice>
#include <QString>

#include <Deuterium/GigSampler/Controls.hpp>

namespace
{
// Distinguishes the current DataStream layout (path, marker, instrument,
// ports) from the released drumkit-only Deuterium's (path, ports): the value
// can never be a port-array header. 'DTM1'.
constexpr int32_t deuteriumStreamMarker = 0x44544D31;

// Documents saved by the released Deuterium carry only the MIDI inlet and
// the audio outlet: create the control inlets they predate. Called from the
// serialization friends, which can reach the inlet list.
void ensureControls(Deuterium::Gig::ProcessModel& proc, Process::Inlets& inlets)
{
  const std::size_t expected = 1 + Deuterium::Gig::ControlCount;
  if(inlets.size() >= expected)
    return;

  const std::size_t have = inlets.size();
  auto all = Deuterium::Gig::makeSamplerControls(&proc);
  for(std::size_t c = 0; c < all.size(); c++)
  {
    if(1 + c < have)
      delete all[c]; // this position was restored from the document
    else
      inlets.push_back(all[c]);
  }
}
}

template <>
void DataStreamReader::read(const Deuterium::Gig::ProcessModel& proc)
{
  m_stream << proc.m_filePath << deuteriumStreamMarker << proc.m_instrument;
  readPorts(*this, proc.m_inlets, proc.m_outlets);

  insertDelimiter();
}

template <>
void DataStreamWriter::write(Deuterium::Gig::ProcessModel& proc)
{
  QString filePath;
  int instrument{};
  m_stream >> filePath;

  // Legacy drumkit-only Deuterium documents continue with the port data
  // right after the path; current ones carry a marker + instrument index
  m_stream_impl.startTransaction();
  int32_t marker{};
  m_stream >> marker;
  if(marker == deuteriumStreamMarker)
  {
    m_stream_impl.commitTransaction();
    m_stream >> instrument;
  }
  else
  {
    m_stream_impl.rollbackTransaction();
  }

  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);
  ensureControls(proc, proc.m_inlets);

  proc.loadFile(filePath, instrument);
  checkDelimiter();
}

template <>
void JSONReader::read(const Deuterium::Gig::ProcessModel& proc)
{
  obj["File"] = proc.m_filePath;
  obj["Instrument"] = proc.m_instrument;
  readPorts(*this, proc.m_inlets, proc.m_outlets);
}

template <>
void JSONWriter::write(Deuterium::Gig::ProcessModel& proc)
{
  QString filePath;
  int instrument{};
  if(auto file = obj.tryGet("File"))
  {
    filePath = file->toString();
    if(auto v = obj.tryGet("Instrument"))
      instrument = v->toInt();
  }
  else if(auto kit = obj.tryGet("Kit"))
  {
    // Document saved by the released drumkit-only Deuterium
    filePath = kit->toString();
  }
  proc.loadFile(filePath, instrument);
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);
  ensureControls(proc, proc.m_inlets);
}
