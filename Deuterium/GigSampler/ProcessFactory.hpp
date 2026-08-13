#pragma once
#include <Process/GenericProcessFactory.hpp>
#include <Process/ProcessFactory.hpp>

#include <Control/DefaultEffectItem.hpp>
#include <Effect/EffectFactory.hpp>
#include <Deuterium/GigSampler/ProcessMetadata.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>

namespace Deuterium::Gig
{
using ProcessFactory = Process::ProcessFactory_T<Deuterium::Gig::ProcessModel>;
}
