#pragma once

#include <Process/Drop/ProcessDropHandler.hpp>

#include <Library/LibraryInterface.hpp>
#include <Library/LibrarySettings.hpp>
#include <Library/ProcessesItemModel.hpp>

#include <QFileInfo>

#include <Deuterium/GigSampler/GigLoader.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>

namespace Deuterium::Gig
{
class LibraryHandler final
    : public QObject
    , public Library::LibraryInterface
{
  SCORE_CONCRETE("b1d2e3f4-5a6b-7c8d-9e0f-a1b2c3d4e5f6")

  QSet<QString> acceptedFiles() const noexcept override
  {
    return {
        "gig", "dls", "sf2", "xml", "kmp",
        //        "wav", "flac", "ogg", "aiff", "aif", "mp3"
    };
  }

  Library::CategoryPaths categories;

  void setup(Library::ProcessesItemModel& model, const score::GUIApplicationContext& ctx)
      override
  {
    categories.init(
        Metadata<PrettyName_k, Deuterium::Gig::ProcessModel>::get().toStdString(), ctx);
  }

  // Worker thread: pure scan. Two category levels: file format first
  // ("GIG", "SF2", ...), then the file's parent folder; multi-instrument
  // files carry one staged child per instrument (the file entry itself
  // plays instrument 0).
  std::optional<Library::ProcessEntry> scanPath(std::string_view path) override
  {
    score::PathInfo file{path};

    // Of the .xml files, only Hydrogen drumkits belong to this sampler
    // (case-insensitive: kits from case-preserving archives vary)
    const auto fileName
        = QString::fromUtf8(file.fileName.data(), file.fileName.size());
    if(fileName.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
       && fileName.compare(QStringLiteral("drumkit.xml"), Qt::CaseInsensitive) != 0)
      return std::nullopt;

    Library::ProcessData pdata;
    pdata.prettyName
        = QString::fromUtf8(file.completeBaseName.data(), file.completeBaseName.size());
    pdata.key = Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get();
    pdata.customData
        = QString::fromUtf8(file.absoluteFilePath.data(), file.absoluteFilePath.size());

    // Enumerating the file's instruments reads no sample data.
    auto instruments = listInstruments(pdata.customData);

    // Unparseable files and instrument-less containers (e.g. GigaPulse
    // impulse-response shells) cannot be played: keep them out of the library
    if(instruments.empty())
      return std::nullopt;

    const auto format = formatName(pdata.customData);

    // Drumkits are named after their folder, not the generic "drumkit.xml"
    if(format == QStringLiteral("Drumkit") && !instruments[0].empty())
      pdata.prettyName = QString::fromStdString(instruments[0]);

    Library::ProcessEntry e;
    e.rootKey = pdata.key;
    e.categoryPath = categoryPath(file, format, fileName);

    e.node.data = std::move(pdata);
    if(instruments.size() > 1)
    {
      const auto& filePath = e.node.data.customData;
      for(std::size_t i = 0; i < instruments.size(); i++)
      {
        Library::ProcessData child;
        child.key = e.rootKey;
        child.prettyName = instruments[i].empty()
                               ? QStringLiteral("Instrument %1").arg(i)
                               : QString::fromStdString(instruments[i]);
        child.customData = filePath + '|' + QString::number(i);
        e.node.children.push_back(Library::StagedNode{std::move(child), {}});
      }
    }
    return e;
  }

  QStringList categoryPath(
      const score::PathInfo& file, const QString& format,
      const QString& fileName) const noexcept
  {
    const auto paths = categories.get();
    if(!paths)
      return {format};

    // Files at the root of the library (or of a preset folder) go directly
    // under the format node
    if(file.absolutePath == paths->packagesRoot
       || file.absolutePath.ends_with(paths->presets))
      return {format};

    // Hydrogen kits are one-per-folder with a fixed file name: their parent
    // folder is the kit itself, so group by the folder above it instead
    if(fileName.compare(QStringLiteral("drumkit.xml"), Qt::CaseInsensitive) == 0)
    {
      score::PathInfo parent{file.absolutePath};
      if(parent.absolutePath == paths->packagesRoot)
        return {format};
      return {format,
              QString::fromUtf8(
                  parent.parentDirName.data(), parent.parentDirName.size())};
    }

    return {format,
            QString::fromUtf8(file.parentDirName.data(), file.parentDirName.size())};
  }
};

class DropHandler final : public Process::ProcessDropHandler
{
  SCORE_CONCRETE("c2e3f4a5-6b7c-8d9e-0f1a-b2c3d4e5f6a7")

  QSet<QString> fileExtensions() const noexcept override
  {
    // Plain audio files are deliberately absent: those drops belong to the
    // Sound process; audio files are still playable by dragging them from
    // this sampler's library section
    return {"gig", "dls", "sf2", "xml", "kmp"};
  }

  void dropPath(
      std::vector<ProcessDrop>& vec, const score::FilePath& filename,
      const score::DocumentContext& ctx) const noexcept override
  {
    const QFileInfo info{filename.absolute};
    const auto ext = info.suffix().toLower();
    if(ext != "gig" && ext != "dls" && ext != "sf2" && ext != "kmp")
    {
      // Of the .xml files, only Hydrogen drumkits are ours
      if(ext != "xml"
         || info.fileName().compare(QStringLiteral("drumkit.xml"), Qt::CaseInsensitive)
                != 0)
        return;
    }

    Process::ProcessDropHandler::ProcessDrop p;
    p.creation.key = Metadata<ConcreteKey_k, ProcessModel>::get();
    p.creation.prettyName
        = info.fileName().compare(QStringLiteral("drumkit.xml"), Qt::CaseInsensitive) == 0
              ? info.dir().dirName()
              : info.baseName();
    p.creation.customData = filename.absolute;

    vec.push_back(std::move(p));
  }
};

}
