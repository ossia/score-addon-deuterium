#pragma once
#include <Process/ProcessMetadata.hpp>

#include <QString>

namespace Deuterium::Gig
{
class ProcessModel;
}

// The UUID is the one of the Deuterium process released in earlier ossia
// score versions (then a Hydrogen-drumkit-only player): documents saved with
// it load into this sampler, see ProcessModelSerialization.cpp.
PROCESS_METADATA(
    , Deuterium::Gig::ProcessModel, "95f8ee65-e418-4f75-b5e4-3e039bb90ac8", "Deuterium",
    "Deuterium", Process::ProcessCategory::Synth, "Audio/Synth",
    "Sampler for GIG/DLS/SF2/KORG banks, Hydrogen drumkits and audio files",
    "ossia score",
    (QStringList{"Script", "Deuterium", "Sampler", "GigSampler"}), {}, {},
    QUrl("https://ossia.io/score-docs/processes/deuterium.html"),
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
