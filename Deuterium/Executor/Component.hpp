#pragma once
#include <Process/Execution/ProcessComponent.hpp>
#include <Process/ExecutionContext.hpp>

#include <score/document/DocumentContext.hpp>
#include <score/document/DocumentInterface.hpp>

#include <ossia/dataflow/node_process.hpp>
#include <ossia/editor/scenario/time_process.hpp>
#include <ossia/editor/scenario/time_value.hpp>

#include <memory>

namespace Deuterium
{
class ProcessModel;
namespace Executor
{
class Component final
    : public ::Execution::ProcessComponent_T<Deuterium::ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("1e6a8939-8cf2-4beb-8d4e-f25b6b29f1e9")
public:
  Component(Deuterium::ProcessModel& element, const Execution::Context& ctx, QObject* parent);
  ~Component() override;

private:
  void on_scriptChange(const QString& script);
};

using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
