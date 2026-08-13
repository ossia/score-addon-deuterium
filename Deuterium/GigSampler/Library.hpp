#pragma once

#include <Process/Drop/ProcessDropHandler.hpp>

#include <Library/LibraryInterface.hpp>
#include <Library/LibrarySettings.hpp>
#include <Library/ProcessesItemModel.hpp>

#include <QFileInfo>

#include <Deuterium/GigSampler/ProcessModel.hpp>

namespace Deuterium::Gig
{
class LibraryHandler final
    : public QObject
    , public Library::LibraryInterface
{
  SCORE_CONCRETE("b1d2e3f4-5a6b-7c8d-9e0f-a1b2c3d4e5f6")
public:
  Library::ProcessNode* node{};

  QSet<QString> acceptedFiles() const noexcept override { return {"gig", "dls", "sf2"}; }

  void setup(Library::ProcessesItemModel& model, const score::GUIApplicationContext& ctx)
      override
  {
    const auto& key = Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get();
    QModelIndex node = model.find(key);
    if(node == QModelIndex{})
      return;
    auto& parent = *reinterpret_cast<Library::ProcessNode*>(node.internalPointer());
    parent.key = {};
    this->node = &parent;
  }

  void addPath(std::string_view path) override
  {
    qDebug() << path;
    score::PathInfo file{path};
    Library::ProcessData pdata;
    pdata.prettyName = QString::fromUtf8(file.baseName);
    pdata.key = Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get();
    pdata.customData = QString::fromUtf8(file.absoluteFilePath);

    Library::addToLibrary(*node, std::move(pdata));
  }
};

class DropHandler final : public Process::ProcessDropHandler
{
  SCORE_CONCRETE("c2e3f4a5-6b7c-8d9e-0f1a-b2c3d4e5f6a7")

  QSet<QString> fileExtensions() const noexcept override { return {"gig", "dls", "sf2"}; }

  void dropPath(
      std::vector<ProcessDrop>& vec, const score::FilePath& filename,
      const score::DocumentContext& ctx) const noexcept override
  {
    const auto ext = QFileInfo(filename.absolute).suffix().toLower();
    if(ext != "gig" && ext != "dls" && ext != "sf2")
      return;

    Process::ProcessDropHandler::ProcessDrop p;
    p.creation.key = Metadata<ConcreteKey_k, ProcessModel>::get();
    p.creation.prettyName = QFileInfo(filename.absolute).baseName();
    p.creation.customData = filename.absolute;

    vec.push_back(std::move(p));
  }
};

}
