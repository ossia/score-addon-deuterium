// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "score_addon_deuterium.hpp"

#include <Process/ProcessFactory.hpp>

#include <Execution/DocumentPlugin.hpp>

#include <score/plugins/FactorySetup.hpp>
#include <score/plugins/InterfaceList.hpp>
#include <score/plugins/StringFactoryKey.hpp>
#include <score/tools/std/HashMap.hpp>

#include <Deuterium/ApplicationPlugin.hpp>
#include <Deuterium/Commands/CommandFactory.hpp>
#include <Deuterium/Executor/Component.hpp>
#include <Deuterium/Library.hpp>
#include <Deuterium/ProcessFactory.hpp>

#include <score_plugin_deuterium_commands_files.hpp>
#include <wobjectimpl.h>

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
