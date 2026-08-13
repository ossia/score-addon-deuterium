#pragma once
#include <Process/ProcessMetadata.hpp>

#include <QString>

namespace Deuterium::Gig
{
class ProcessModel;
}

PROCESS_METADATA(
    , Deuterium::Gig::ProcessModel, "a7c3e1f2-8b4d-4e9a-b5c6-d2f1e3a4b5c6", "GigSampler",
    "GigSampler", Process::ProcessCategory::Synth, "Audio/Synth",
    "Sampler for GIG/DLS/SF2 files (libgig)", "ossia score",
    (QStringList{"Script", "GigSampler", "Sampler"}), {}, {},
    QUrl("https://ossia.io/score-docs/processes/gigsampler.html"),
    Process::ProcessFlags::SupportsAll | Process::ProcessFlags::PutInNewSlot
        | Process::ProcessFlags::ControlSurface)
