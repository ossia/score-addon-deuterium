#include "GigLoader.hpp"

#include <QFileInfo>

#include <gig.h>

#include <cmath>
#include <cstring>

namespace Deuterium::Gig
{

// Converts raw PCM buffer (already loaded via LoadSampleData) into double arrays
// and resamples to targetRate. Called per-sample from the task pool.
static void convertRawSampleData(
    const void* rawData, int64_t rawSize, int channels, int bitDepth,
    int64_t totalSamples, GigSample& out, int targetRate)
{
  if(!rawData || rawSize == 0 || totalSamples == 0)
    return;

  const int outChannels = std::max(1, channels);
  out.data.resize(outChannels);
  for(auto& ch : out.data)
    ch.resize(totalSamples);

  const auto* raw = static_cast<const uint8_t*>(rawData);

  if(bitDepth == 16)
  {
    const auto* samples = reinterpret_cast<const int16_t*>(raw);
    constexpr double scale = 1.0 / 32768.0;
    if(channels == 1)
    {
      for(int64_t i = 0; i < totalSamples; i++)
        out.data[0][i] = samples[i] * scale;
    }
    else if(channels == 2)
    {
      for(int64_t i = 0; i < totalSamples; i++)
      {
        out.data[0][i] = samples[i * 2] * scale;
        out.data[1][i] = samples[i * 2 + 1] * scale;
      }
    }
  }
  else if(bitDepth == 24)
  {
    constexpr double scale = 1.0 / 8388608.0;
    if(channels == 1)
    {
      for(int64_t i = 0; i < totalSamples; i++)
      {
        int32_t s = (int32_t)raw[i * 3] | ((int32_t)raw[i * 3 + 1] << 8)
                    | ((int32_t)(int8_t)raw[i * 3 + 2] << 16);
        out.data[0][i] = s * scale;
      }
    }
    else if(channels == 2)
    {
      for(int64_t i = 0; i < totalSamples; i++)
      {
        int64_t off = i * 6;
        int32_t l = (int32_t)raw[off] | ((int32_t)raw[off + 1] << 8)
                    | ((int32_t)(int8_t)raw[off + 2] << 16);
        int32_t r = (int32_t)raw[off + 3] | ((int32_t)raw[off + 4] << 8)
                    | ((int32_t)(int8_t)raw[off + 5] << 16);
        out.data[0][i] = l * scale;
        out.data[1][i] = r * scale;
      }
    }
  }
  else if(bitDepth == 8)
  {
    constexpr double scale = 1.0 / 128.0;
    if(channels == 1)
    {
      for(int64_t i = 0; i < totalSamples; i++)
        out.data[0][i] = ((int)raw[i] - 128) * scale;
    }
    else if(channels == 2)
    {
      for(int64_t i = 0; i < totalSamples; i++)
      {
        out.data[0][i] = ((int)raw[i * 2] - 128) * scale;
        out.data[1][i] = ((int)raw[i * 2 + 1] - 128) * scale;
      }
    }
  }

  // Resample to target rate if needed
  if(out.sampleRate != (uint32_t)targetRate && targetRate > 0)
  {
    const double ratio = (double)targetRate / (double)out.sampleRate;
    const int64_t newLen = (int64_t)(totalSamples * ratio);
    if(newLen > 0)
    {
      ossia::audio_array resampled;
      resampled.resize(outChannels);
      for(int c = 0; c < outChannels; c++)
      {
        resampled[c].resize(newLen);
        for(int64_t i = 0; i < newLen; i++)
        {
          double srcPos = i / ratio;
          int64_t idx = (int64_t)srcPos;
          double frac = srcPos - idx;
          if(idx + 1 < totalSamples)
            resampled[c][i]
                = out.data[c][idx] * (1.0 - frac) + out.data[c][idx + 1] * frac;
          else if(idx < totalSamples)
            resampled[c][i] = out.data[c][idx];
          else
            resampled[c][i] = 0.0;
        }
      }
      out.data = std::move(resampled);

      // Update loop points
      if(out.hasLoop)
      {
        out.loopStart = (uint32_t)(out.loopStart * ratio);
        out.loopEnd = (uint32_t)(out.loopEnd * ratio);
      }
    }
    out.sampleRate = targetRate;
  }
}

std::shared_ptr<GigFileInfo>
loadGigFileMetadata(const QString& filePath, int instrumentIndex)
{
  try
  {
    auto riff = std::make_unique<RIFF::File>(filePath.toStdString());
    auto gigFile = std::make_unique<gig::File>(riff.get());

    auto info = std::make_shared<GigFileInfo>();
    info->filePath = filePath.toStdString();
    info->selectedInstrument = instrumentIndex;

    if(gigFile->pInfo)
      info->name = gigFile->pInfo->Name;
    if(info->name.empty())
      info->name = QFileInfo(filePath).baseName().toStdString();

    // Load all instruments metadata
    gig::Instrument* gigInstr = gigFile->GetFirstInstrument();
    while(gigInstr)
    {
      GigInstrument instr;
      if(gigInstr->pInfo)
        instr.name = gigInstr->pInfo->Name;
      instr.attenuation = gigInstr->Attenuation;
      instr.pitchbendRange = gigInstr->PitchbendRange;

      info->instruments.push_back(std::move(instr));
      gigInstr = gigFile->GetNextInstrument();
    }

    if(info->instruments.empty())
      return {};

    // Clamp instrument index
    if(instrumentIndex < 0 || instrumentIndex >= (int)info->instruments.size())
      instrumentIndex = 0;
    info->selectedInstrument = instrumentIndex;

    // Parse the selected instrument's regions (metadata only, no sample data)
    gigInstr = gigFile->GetInstrument(instrumentIndex);
    if(!gigInstr)
      return {};

    auto& destInstr = info->instruments[instrumentIndex];

    gig::Region* rgn = gigInstr->GetFirstRegion();
    while(rgn)
    {
      for(uint32_t d = 0; d < rgn->DimensionRegions; d++)
      {
        gig::DimensionRegion* dimRgn = rgn->pDimensionRegions[d];
        if(!dimRgn || !dimRgn->pSample)
          continue;

        // Skip samples with no data
        if(dimRgn->pSample->SamplesTotal == 0)
          continue;

        GigRegion region;
        region.keyLow = rgn->KeyRange.low;
        region.keyHigh = rgn->KeyRange.high;
        region.velLow = 0;
        region.velHigh = dimRgn->VelocityUpperLimit > 0 ? dimRgn->VelocityUpperLimit : 127;

        // Determine velocity range from dimension definitions
        for(unsigned int dim = 0; dim < rgn->Dimensions; dim++)
        {
          if(rgn->pDimensionDefinitions[dim].dimension == gig::dimension_velocity)
          {
            int bitsBelow = 0;
            for(unsigned int dd = 0; dd < dim; dd++)
              bitsBelow += rgn->pDimensionDefinitions[dd].bits;

            int dimBits = rgn->pDimensionDefinitions[dim].bits;
            int mask = (1 << dimBits) - 1;
            int zone = (d >> bitsBelow) & mask;

            if(zone > 0 && dimRgn->DimensionUpperLimits[dim] > 0)
            {
              region.velLow = (dimRgn->DimensionUpperLimits[dim] * zone
                               / rgn->pDimensionDefinitions[dim].zones);
            }
            if(dimRgn->DimensionUpperLimits[dim] > 0)
              region.velHigh = dimRgn->DimensionUpperLimits[dim];
            break;
          }
        }

        // EG1 (Amplitude envelope)
        region.eg1Attack = dimRgn->EG1Attack;
        region.eg1Decay = dimRgn->EG1Decay1;
        region.eg1Sustain = dimRgn->EG1Sustain / 1000.0; // 0-1000 -> 0.0-1.0
        region.eg1Release = dimRgn->EG1Release;

        // Filter
        region.vcfEnabled = dimRgn->VCFEnabled;
        region.vcfCutoff = dimRgn->VCFCutoff;
        region.vcfResonance = dimRgn->VCFResonance;

        // Playback
        region.pitchTrack = dimRgn->PitchTrack;
        region.pan = dimRgn->Pan;
        region.sampleAttenuation = dimRgn->SampleAttenuation;
        region.sampleStartOffset = dimRgn->SampleStartOffset;

        // Sample metadata (but NOT the actual audio data)
        auto* smp = dimRgn->pSample;
        region.sample.sampleRate = smp->SamplesPerSecond;
        region.sample.midiUnityNote = smp->MIDIUnityNote;
        region.sample.fineTune = smp->FineTune;
        if(smp->Loops > 0)
        {
          region.sample.hasLoop = true;
          region.sample.loopStart = smp->LoopStart;
          region.sample.loopEnd = smp->LoopEnd;
          region.sample.loopType = static_cast<int>(smp->LoopType);
        }
        // sample.data left empty intentionally

        destInstr.regions.push_back(std::move(region));
      }

      rgn = gigInstr->GetNextRegion();
    }

    return info;
  }
  catch(RIFF::Exception& e)
  {
    qWarning() << "libgig metadata error:" << e.Message.c_str();
    return {};
  }
  catch(...)
  {
    qWarning() << "Unknown error parsing GIG metadata:" << filePath;
    return {};
  }
}

std::shared_ptr<GigFileInfo> loadGigFileSamples(
    const std::shared_ptr<GigFileInfo>& metadata,
    int targetRate,
    std::shared_ptr<std::atomic<bool>> cancelled)
{
  if(!metadata)
    return {};

  try
  {
    // Make a deep copy of the metadata so we can fill in sample data
    // without racing with the GUI thread
    auto info = std::make_shared<GigFileInfo>(*metadata);

    auto riff
        = std::make_unique<RIFF::File>(QString::fromStdString(info->filePath).toStdString());
    auto gigFile = std::make_unique<gig::File>(riff.get());

    const int instrIdx = info->selectedInstrument;
    if(instrIdx < 0 || instrIdx >= (int)info->instruments.size())
      return {};

    gig::Instrument* gigInstr = gigFile->GetInstrument(instrIdx);
    if(!gigInstr)
      return {};

    auto& destInstr = info->instruments[instrIdx];

    // We need to walk the gig regions in the same order as loadGigFileMetadata
    // to match region indices
    struct RawSampleBuffer
    {
      std::vector<uint8_t> data;
      int channels{};
      int bitDepth{};
      int64_t totalSamples{};
      int regionIndex{};
    };

    // Phase 2a: Read all raw sample data from libgig (single-threaded, libgig not thread-safe)
    std::vector<RawSampleBuffer> rawBuffers;
    rawBuffers.reserve(destInstr.regions.size());

    int regionIdx = 0;
    gig::Region* rgn = gigInstr->GetFirstRegion();
    while(rgn)
    {
      if(cancelled && cancelled->load(std::memory_order_relaxed))
        return {};

      for(uint32_t d = 0; d < rgn->DimensionRegions; d++)
      {
        gig::DimensionRegion* dimRgn = rgn->pDimensionRegions[d];
        if(!dimRgn || !dimRgn->pSample)
          continue;

        auto* smp = dimRgn->pSample;
        if(smp->SamplesTotal == 0)
          continue;

        if(regionIdx >= (int)destInstr.regions.size())
          break;

        // Load raw sample data from the gig file
        gig::buffer_t buf = smp->LoadSampleData();
        if(buf.pStart && buf.Size > 0)
        {
          RawSampleBuffer raw;
          raw.data.resize(buf.Size);
          std::memcpy(raw.data.data(), buf.pStart, buf.Size);
          raw.channels = smp->Channels;
          raw.bitDepth = smp->BitDepth;
          raw.totalSamples = smp->SamplesTotal;
          raw.regionIndex = regionIdx;
          rawBuffers.push_back(std::move(raw));
        }
        smp->ReleaseSampleData();

        regionIdx++;
      }

      rgn = gigInstr->GetNextRegion();
    }

    // Close libgig/RIFF - no longer needed
    gigFile.reset();
    riff.reset();

    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return {};

    // Phase 2b: Convert and resample each sample.
    // Done sequentially here because this function already runs on a
    // TaskPool worker — posting back to the same pool and busy-waiting
    // would risk deadlock, and returning early on cancellation while
    // posted tasks still reference our locals would be use-after-free.
    for(auto& raw : rawBuffers)
    {
      if(cancelled && cancelled->load(std::memory_order_relaxed))
        return {};

      auto& region = destInstr.regions[raw.regionIndex];
      convertRawSampleData(
          raw.data.data(), raw.data.size(), raw.channels, raw.bitDepth,
          raw.totalSamples, region.sample, targetRate);
    }

    // Remove regions that still have no sample data (failed to load)
    auto beforePrune = destInstr.regions.size();
    std::erase_if(destInstr.regions, [](const GigRegion& r) {
      return r.sample.data.empty() || r.sample.data[0].empty();
    });

    qWarning() << "GigSampler: loadGigFileSamples completed."
               << "rawBuffers:" << rawBuffers.size()
               << "regions before prune:" << beforePrune
               << "after prune:" << destInstr.regions.size();

    return info;
  }
  catch(RIFF::Exception& e)
  {
    qWarning() << "libgig sample loading error:" << e.Message.c_str();
    return {};
  }
  catch(...)
  {
    qWarning() << "Unknown error loading GIG samples";
    return {};
  }
}

}
