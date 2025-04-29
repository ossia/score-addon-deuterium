#pragma once

#include <Process/Drop/ProcessDropHandler.hpp>

#include <Library/LibraryInterface.hpp>
#include <Library/LibrarySettings.hpp>
#include <Library/ProcessesItemModel.hpp>

#include <QFileInfo>

#include <Deuterium/ProcessModel.hpp>

namespace Deuterium
{
class LibraryHandler final
    : public QObject
    , public Library::LibraryInterface
{
  SCORE_CONCRETE("10e62eb7-2173-4c3a-aae2-0a9882f553aa")
public:
  Library::ProcessNode* node{};

  QSet<QString> acceptedFiles() const noexcept override { return {"xml"}; }

  void setup(Library::ProcessesItemModel& model, const score::GUIApplicationContext& ctx)
      override
  {
    // TODO relaunch whenever library path changes...
    const auto& key = Metadata<ConcreteKey_k, Deuterium::ProcessModel>::get();
    QModelIndex node = model.find(key);
    if(node == QModelIndex{})
      return;
    auto& parent = *reinterpret_cast<Library::ProcessNode*>(node.internalPointer());
    parent.key = {};
    this->node = &parent;
  }

  void addPath(std::string_view path) override
  {
    score::PathInfo file{path};
    if(file.fileName != "drumkit.xml")
      return;

    Library::ProcessData pdata;
    pdata.prettyName = QString::fromUtf8(file.parentDirName);
    pdata.key = Metadata<ConcreteKey_k, Deuterium::ProcessModel>::get();
    pdata.customData = QString::fromUtf8(file.absoluteFilePath);

    Library::addToLibrary(*node, std::move(pdata));
  }
};

class DropHandler final : public Process::ProcessDropHandler
{
  SCORE_CONCRETE("a76f82c4-59ca-410a-81db-141444d4613d")

  QSet<QString> fileExtensions() const noexcept override { return {"xml"}; }

  void dropPath(
      std::vector<ProcessDrop>& vec, const score::FilePath& filename,
      const score::DocumentContext& ctx) const noexcept override
  {
    if(filename.filename != "drumkit.xml")
      return;

    Process::ProcessDropHandler::ProcessDrop p;
    p.creation.key = Metadata<ConcreteKey_k, ProcessModel>::get();
    p.creation.prettyName = QString::fromUtf8(
        score::PathInfo{filename.absolute.toStdString()}.parentDirName);
    p.creation.customData = filename.absolute;

    vec.push_back(std::move(p));
  }
};

}
