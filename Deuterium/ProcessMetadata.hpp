#pragma once
#include <Process/ProcessMetadata.hpp>

#include <QString>

namespace Deuterium
{
class ProcessModel;
}

PROCESS_METADATA(
    , Deuterium::ProcessModel, "95f8ee65-e418-4f75-b5e4-3e039bb90ac8", "Deuterium",
    "Deuterium", Process::ProcessCategory::Synth, "Audio/Synth", "Hydrogen drums",
    "ossia score", (QStringList{"Script", "Deuterium"}), {}, {},
    QUrl("https://ossia.io/score-docs/processes/deuterium.html"),
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
