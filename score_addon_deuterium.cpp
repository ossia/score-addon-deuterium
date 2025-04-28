// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "score_addon_deuterium.hpp"

#include <Process/Drop/ProcessDropHandler.hpp>
#include <Process/ProcessFactory.hpp>

#include <Execution/DocumentPlugin.hpp>
#include <Library/LibraryInterface.hpp>
#include <Library/LibrarySettings.hpp>
#include <Library/ProcessesItemModel.hpp>

#include <score/plugins/FactorySetup.hpp>
#include <score/plugins/InterfaceList.hpp>
#include <score/plugins/StringFactoryKey.hpp>
#include <score/tools/std/HashMap.hpp>

#include <QFileInfo>
#include <QQmlListProperty>
#include <QTimer>

#include <Deuterium/ApplicationPlugin.hpp>
#include <Deuterium/Commands/CommandFactory.hpp>
#include <Deuterium/Executor/Component.hpp>
#include <Deuterium/ProcessFactory.hpp>

#include <score_plugin_deuterium_commands_files.hpp>
#include <wobjectimpl.h>

namespace Deuterium
{
class LibraryHandler final
    : public QObject
    , public Library::LibraryInterface
{
  SCORE_CONCRETE("10e62eb7-2173-4c3a-aae2-0a9882f553aa")
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

score_plugin_deuterium::score_plugin_deuterium() { }

score_plugin_deuterium::~score_plugin_deuterium() = default;

std::vector<score::InterfaceBase*> score_plugin_deuterium::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext,
      FW<Process::ProcessModelFactory, Deuterium::ProcessFactory>,
      //FW<Process::LayerFactory, Deuterium::LayerFactory>,
      FW<Library::LibraryInterface, Deuterium::LibraryHandler>,
      FW<Process::ProcessDropHandler, Deuterium::DropHandler>,
      FW<Execution::ProcessComponentFactory, Deuterium::Executor::ComponentFactory>>(
      ctx, key);
}

std::pair<const CommandGroupKey, CommandGeneratorMap> score_plugin_deuterium::make_commands()
{
  using namespace Deuterium;
  std::pair<const CommandGroupKey, CommandGeneratorMap> cmds{
      Deuterium::CommandFactoryName(), CommandGeneratorMap{}};

  ossia::for_each_type<
#include <score_plugin_deuterium_commands.hpp>
      >(score::commands::FactoryInserter{cmds.second});

  return cmds;
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_plugin_deuterium)
