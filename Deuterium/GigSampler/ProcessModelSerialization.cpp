#include "ProcessModel.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>

#include <score/application/ApplicationComponents.hpp>
#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONValueVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>

#include <QIODevice>
#include <QString>

template <>
void DataStreamReader::read(const Deuterium::Gig::ProcessModel& proc)
{
  m_stream << proc.m_filePath << proc.m_instrument;
  readPorts(*this, proc.m_inlets, proc.m_outlets);

  insertDelimiter();
}

template <>
void DataStreamWriter::write(Deuterium::Gig::ProcessModel& proc)
{
  QString filePath;
  int instrument{};
  m_stream >> filePath >> instrument;
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);

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
  QString filePath = obj["File"].toString();
  int instrument{};
  if(auto v = obj.tryGet("Instrument"))
    instrument = v->toInt();
  proc.loadFile(filePath, instrument);
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);
}
