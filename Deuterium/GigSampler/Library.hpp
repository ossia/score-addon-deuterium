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
    return {"gig", "dls", "sf2",  "xml", "kmp",
            "wav", "flac", "ogg", "aiff", "aif", "mp3"};
  }

  Library::Subcategories categories;
  ossia::hash_map<QString, Library::ProcessNode*> formatNodes;
  ossia::hash_map<QString, Library::ProcessNode*> folderNodes;

  void setup(Library::ProcessesItemModel& model, const score::GUIApplicationContext& ctx)
      override
  {
    // A rescan rebuilds the whole node tree: the cached raw pointers of the
    // previous scan would dangle
    formatNodes.clear();
    folderNodes.clear();

    const auto& key = Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get();
    QModelIndex node = model.find(key);
    if(node == QModelIndex{})
      return;

    categories.init(
        Metadata<PrettyName_k, Deuterium::Gig::ProcessModel>::get().toStdString(), node,
        ctx);

  }

  std::function<void()> asyncAddPath(std::string_view path) override
  {
    score::PathInfo file{path};

    // Of the .xml files, only Hydrogen drumkits belong to this sampler
    // (case-insensitive: kits from case-preserving archives vary)
    const auto fileName
        = QString::fromUtf8(file.fileName.data(), file.fileName.size());
    if(fileName.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)
       && fileName.compare(QStringLiteral("drumkit.xml"), Qt::CaseInsensitive) != 0)
      return {};

    Library::ProcessData pdata;
    pdata.prettyName
        = QString::fromUtf8(file.completeBaseName.data(), file.completeBaseName.size());
    pdata.key = Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get();
    pdata.customData
        = QString::fromUtf8(file.absoluteFilePath.data(), file.absoluteFilePath.size());

    // We are on a worker thread: enumerating the file's instruments (no
    // sample data is read) is fine here, but the model may only be touched
    // from the returned continuation, which runs on the GUI thread.
    auto instruments = listInstruments(pdata.customData);

    // Unparseable files and instrument-less containers (e.g. GigaPulse
    // impulse-response shells) cannot be played: keep them out of the library
    if(instruments.empty())
      return {};

    auto format = formatName(pdata.customData);

    // Drumkits are named after their folder, not the generic "drumkit.xml"
    if(format == QStringLiteral("Drumkit") && !instruments[0].empty())
      pdata.prettyName = QString::fromStdString(instruments[0]);

    return [this, p = std::string(path), pdata = std::move(pdata),
            format = std::move(format),
            instruments = std::move(instruments)]() mutable {
      score::PathInfo file{p};
      const auto key = pdata.key;
      const auto filePath = pdata.customData;

      auto& fileNode = addToCategory(file, format, std::move(pdata));

      // Multi-instrument files additionally expose one child per instrument
      // (the file entry itself plays instrument 0)
      if(instruments.size() > 1)
      {
        for(std::size_t i = 0; i < instruments.size(); i++)
        {
          Library::ProcessData child;
          child.key = key;
          child.prettyName = instruments[i].empty()
                                 ? QStringLiteral("Instrument %1").arg(i)
                                 : QString::fromStdString(instruments[i]);
          child.customData = filePath + '|' + QString::number(i);
          Library::addToLibrary(fileNode, std::move(child));
        }
      }
    };
  }

  // Two category levels: file format first ("GIG", "SF2", ...), then the
  // file's parent folder, as Library::Subcategories does. Returns the file's
  // node so that instrument children can be attached to it.
  Library::ProcessNode& addToCategory(
      const score::PathInfo& file, const QString& format,
      Library::ProcessData&& pdata)
  {
    SCORE_ASSERT(categories.parent);

    Library::ProcessNode* fmtNode{};
    if(auto it = formatNodes.find(format); it != formatNodes.end())
    {
      fmtNode = it->second;
    }
    else
    {
      fmtNode = &Library::addToLibrary(
          *categories.parent, Library::ProcessData{{{}, format, {}}, {}});
      formatNodes[format] = fmtNode;
    }

    auto parentFolder
        = QString::fromUtf8(file.parentDirName.data(), file.parentDirName.size());

    // Files at the root of the library (or of a preset folder) go directly
    // under the format node
    if(file.absolutePath == categories.libraryFolderPath
       || file.absolutePath.ends_with(categories.defaultPresetsPath))
      return Library::addToLibrary(*fmtNode, std::move(pdata));

    // Hydrogen kits are one-per-folder with a fixed file name: their parent
    // folder is the kit itself, so group by the folder above it instead
    const auto fn = QString::fromUtf8(file.fileName.data(), file.fileName.size());
    if(fn.compare(QStringLiteral("drumkit.xml"), Qt::CaseInsensitive) == 0)
    {
      score::PathInfo parent{file.absolutePath};
      parentFolder = QString::fromUtf8(
          parent.parentDirName.data(), parent.parentDirName.size());
      if(parent.absolutePath == categories.libraryFolderPath)
        return Library::addToLibrary(*fmtNode, std::move(pdata));
    }

    const QString folderKey = format + QLatin1Char('/') + parentFolder;
    if(auto it = folderNodes.find(folderKey); it != folderNodes.end())
      return Library::addToLibrary(*it->second, std::move(pdata));

    auto& category = Library::addToLibrary(
        *fmtNode, Library::ProcessData{{{}, parentFolder, {}}, {}});
    auto& node = Library::addToLibrary(category, std::move(pdata));
    folderNodes[folderKey] = &category;
    return node;
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
