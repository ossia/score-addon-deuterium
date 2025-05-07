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
#include <Deuterium/Executor/Component.hpp>
#include <Deuterium/Library.hpp>
#include <Deuterium/ProcessFactory.hpp>

#include <score_plugin_deuterium_commands_files.hpp>
#include <wobjectimpl.h>

score_addon_deuterium::score_addon_deuterium() { }

score_addon_deuterium::~score_addon_deuterium() = default;

std::vector<score::InterfaceBase*> score_addon_deuterium::factories(
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

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_deuterium)
