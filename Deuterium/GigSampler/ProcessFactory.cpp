#include "ProcessFactory.hpp"

#include <QStringList>

namespace Deuterium::Gig
{
// A bank is credited to the people the bank itself names, not to the
// sampler that plays it.
Process::Descriptor ProcessFactory::descriptor(QString data) const noexcept
{
  auto desc = Process::ProcessFactory_T<ProcessModel>::descriptor(data);
  if(data.isEmpty())
    return desc;

  const auto credits = bankCredits(parseInstrumentPath(data).file);
  desc.author = credits.author;

  QStringList lines;
  lines.push_back(credits.info.isEmpty() ? desc.description : credits.info);
  if(!credits.license.isEmpty())
    lines.push_back(QStringLiteral("License: ") + credits.license);
  if(!credits.copyright.isEmpty())
    lines.push_back(QStringLiteral("Copyright: ") + credits.copyright);
  desc.description = lines.join('\n');
  return desc;
}
}
