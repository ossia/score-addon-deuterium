// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "score_addon_deuterium.hpp"

#include <Process/ProcessFactory.hpp>

#include <Execution/DocumentPlugin.hpp>

#include <score/plugins/FactorySetup.hpp>
#include <score/plugins/InterfaceList.hpp>
#include <score/plugins/StringFactoryKey.hpp>
#include <score/tools/std/HashMap.hpp>

#include <Deuterium/GigSampler/Executor/Component.hpp>
#include <Deuterium/GigSampler/Layer.hpp>
#include <Deuterium/GigSampler/Library.hpp>
#include <Deuterium/GigSampler/ProcessFactory.hpp>

#include <wobjectimpl.h>

score_addon_deuterium::score_addon_deuterium() { }

score_addon_deuterium::~score_addon_deuterium() = default;

std::vector<score::InterfaceBase*> score_addon_deuterium::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext,
      FW<Process::ProcessModelFactory, Deuterium::Gig::ProcessFactory>,
      FW<Process::LayerFactory, Deuterium::Gig::LayerFactory>,
      FW<Library::LibraryInterface, Deuterium::Gig::LibraryHandler>,
      FW<Process::ProcessDropHandler, Deuterium::Gig::DropHandler>,
      FW<Execution::ProcessComponentFactory, Deuterium::Gig::Executor::ComponentFactory>>(
      ctx, key);
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_deuterium)
