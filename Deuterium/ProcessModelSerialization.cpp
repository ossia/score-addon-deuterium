// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
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
void DataStreamReader::read(const Deuterium::ProcessModel& proc)
{
  // m_stream << proc.m_script << readDeuteriumState(*proc.fx.get());

  readPorts(*this, proc.m_inlets, proc.m_outlets);

  insertDelimiter();
}

template <>
void DataStreamWriter::write(Deuterium::ProcessModel& proc)
{
  /*
  QString str;
  QByteArray dat;
  m_stream >> str >> dat;
  proc.setScript(str);

  loadDeuteriumState(*proc.fx.get(), dat);

*/
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);

  checkDelimiter();
}

template <>
void JSONReader::read(const Deuterium::ProcessModel& proc)
{
  /*
  obj["Script"] = proc.script();
  obj["Chunk"] = readDeuteriumState(*proc.fx.get()).toBase64();
  */
  readPorts(*this, proc.m_inlets, proc.m_outlets);
}

template <>
void JSONWriter::write(Deuterium::ProcessModel& proc)
{ /*
  proc.setScript(obj["Script"].toString());
  auto dat = QByteArray::fromBase64(obj["Chunk"].toByteArray());
  loadDeuteriumState(*proc.fx.get(), dat);
*/
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);
}
