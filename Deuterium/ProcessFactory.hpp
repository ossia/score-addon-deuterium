#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>
#include <Process/Script/ScriptEditor.hpp>
#include <Process/WidgetLayer/WidgetProcessFactory.hpp>

#include <Control/DefaultEffectItem.hpp>
#include <Effect/EffectFactory.hpp>
#include <Deuterium/ProcessMetadata.hpp>
#include <Deuterium/ProcessModel.hpp>

namespace Deuterium
{
struct LanguageSpec
{
  static constexpr const char* language = "Deuterium";
};

using ProcessFactory = Process::ProcessFactory_T<Deuterium::ProcessModel>;
// using LayerFactory = Process::EffectLayerFactory_T<
//     ProcessModel,
//     Process::DefaultEffectItem,
//     Process::ProcessScriptEditDialog<
//         ProcessModel,
//         ProcessModel::p_script,
//         LanguageSpec>>;
}
