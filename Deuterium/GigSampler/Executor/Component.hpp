#pragma once
#include <Process/Execution/ProcessComponent.hpp>
#include <Process/ExecutionContext.hpp>

#include <score/document/DocumentContext.hpp>
#include <score/document/DocumentInterface.hpp>

#include <ossia/dataflow/node_process.hpp>
#include <ossia/editor/scenario/time_process.hpp>
#include <ossia/editor/scenario/time_value.hpp>

#include <memory>

namespace Deuterium::Gig
{
class ProcessModel;
namespace Executor
{
class Component final
    : public ::Execution::ProcessComponent_T<Deuterium::Gig::ProcessModel, ossia::node_process>
{
  COMPONENT_METADATA("d4e5f6a7-8b9c-0d1e-2f3a-b4c5d6e7f8a9")
public:
  Component(
      Deuterium::Gig::ProcessModel& element, const Execution::Context& ctx,
      QObject* parent);
  ~Component() override;
};

using ComponentFactory = ::Execution::ProcessComponentFactory_T<Component>;
}
}
