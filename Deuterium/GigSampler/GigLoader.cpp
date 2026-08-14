#include "GigLoader.hpp"

#include <Media/AudioDecoder.hpp>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

#include <DLS.h>
#include <SF.h>
#include <gig.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Deuterium::Gig
{
namespace
{

enum class SampleFileFormat
{
  Gig,
  Dls,
  Sf2,
  Hydrogen
};

SampleFileFormat formatForPath(const QString& filePath)
{
  const auto extFormat = [&] {
    const auto ext = QFileInfo(filePath).suffix().toLower();
    if(ext == "dls")
      return SampleFileFormat::Dls;
    if(ext == "sf2")
      return SampleFileFormat::Sf2;
    if(ext == "xml")
      return SampleFileFormat::Hydrogen;
    return SampleFileFormat::Gig;
  };

  // Prefer content sniffing: sample banks in the wild frequently carry the
  // wrong extension (e.g. SoundFonts renamed to .dls). Both plain DLS and
  // gig files use the 'DLS ' RIFF form, so there the extension decides
  // whether the richer gig parser is used.
  if(QFile f{filePath}; f.open(QIODevice::ReadOnly))
  {
    char hdr[12]{};
    if(f.read(hdr, 12) == 12 && std::memcmp(hdr, "RIFF", 4) == 0)
    {
      if(std::memcmp(hdr + 8, "sfbk", 4) == 0)
        return SampleFileFormat::Sf2;
      if(std::memcmp(hdr + 8, "DLS ", 4) == 0)
        return extFormat() == SampleFileFormat::Dls ? SampleFileFormat::Dls
                                                    : SampleFileFormat::Gig;
    }
  }
  return extFormat();
}

// Envelope stage lengths end up as playback state: keep them finite and sane
// whatever the file said. Mirrors the gig format's documented 0-60s range.
void sanitizeRegion(GigRegion& r)
{
  const auto env = [](double v) { return std::isfinite(v) ? std::clamp(v, 0., 60.) : 0.; };
  r.eg1Attack = env(r.eg1Attack);
  r.eg1Decay = env(r.eg1Decay);
  r.eg1Release = env(r.eg1Release);
  r.eg1Sustain = std::isfinite(r.eg1Sustain) ? std::clamp(r.eg1Sustain, 0., 1.) : 1.;
  r.sampleAttenuation
      = std::isfinite(r.sampleAttenuation) ? std::clamp(r.sampleAttenuation, 0., 8.) : 1.;
  r.pitchOffset
      = std::isfinite(r.pitchOffset) ? std::clamp(r.pitchOffset, -60., 60.) : 0.;
  r.randomPitch
      = std::isfinite(r.randomPitch) ? std::clamp(r.randomPitch, 0., 12.) : 0.;
  r.sample.fineTune = std::clamp(r.sample.fineTune, -1200, 1200);
}

// Raw PCM read back from a file, waiting to be converted to double arrays
struct RawSampleBuffer
{
  std::vector<uint8_t> data;
  int channels{};
  int bitDepth{};
  int64_t totalSamples{};
  int regionIndex{};
};

// Converts raw PCM buffer (already loaded via LoadSampleData) into double arrays
// and resamples to targetRate.
void convertRawSampleData(
    const void* rawData, int64_t rawSize, int channels, int bitDepth,
    int64_t totalSamples, GigSample& out, int targetRate)
{
  if(!rawData || rawSize <= 0 || totalSamples <= 0)
    return;
  if(bitDepth != 8 && bitDepth != 16 && bitDepth != 24)
    return;

  const int outChannels = std::max(1, channels);

  // The file libraries do not guarantee that LoadSampleData() caches as many
  // frames as the headers advertise (truncated / corrupt files): never trust
  // the header over the byte count actually returned.
  const int64_t frameSize = int64_t(outChannels) * (bitDepth / 8);
  totalSamples = std::min(totalSamples, rawSize / frameSize);
  if(totalSamples <= 0)
    return;

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

  // Never trust the header sample rate: 0 would divide to an infinite ratio
  // below (with an UB float->int cast), and absurd values would make the
  // resampled allocation explode
  if(out.sampleRate < 4000 || out.sampleRate > 768000)
    out.sampleRate = (uint32_t)targetRate;

  // Resample to target rate if needed
  if(out.sampleRate != (uint32_t)targetRate && targetRate > 0)
  {
    const double ratio = (double)targetRate / (double)out.sampleRate;
    const int64_t newLen = (int64_t)(totalSamples * ratio);
    // The upper bound caps the resampled buffer at 2 GB per channel
    if(newLen > 0 && newLen < (int64_t(1) << 28))
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

  // Clamp loop points to the decoded frame count: files can declare
  // LoopStart/LoopEnd past the actual sample data, and the render loop
  // indexes the buffer with these values.
  if(out.hasLoop && !out.data.empty())
  {
    const int64_t frames = (int64_t)out.data[0].size();
    if((int64_t)out.loopEnd > frames)
      out.loopEnd = (uint32_t)frames;
    if(out.loopStart >= out.loopEnd)
      out.hasLoop = false;
  }
}

// Convert raw buffers into the destination regions, then drop regions
// that ended up without sample data.
void convertAndPrune(
    std::vector<RawSampleBuffer>& rawBuffers, GigInstrument& destInstr,
    int targetRate, const std::shared_ptr<std::atomic<bool>>& cancelled,
    bool& wasCancelled)
{
  // Done sequentially here because this function already runs on a
  // TaskPool worker — posting back to the same pool and busy-waiting
  // would risk deadlock, and returning early on cancellation while
  // posted tasks still reference our locals would be use-after-free.
  wasCancelled = false;
  for(auto& raw : rawBuffers)
  {
    if(cancelled && cancelled->load(std::memory_order_relaxed))
    {
      wasCancelled = true;
      return;
    }

    auto& region = destInstr.regions[raw.regionIndex];
    const double originalRate = region.sample.sampleRate;
    convertRawSampleData(
        raw.data.data(), raw.data.size(), raw.channels, raw.bitDepth,
        raw.totalSamples, region.sample, targetRate);

    // Loop points are rescaled inside the conversion; the start offset is
    // a region property, rescale it here
    if(region.sampleStartOffset > 0 && originalRate > 0
       && region.sample.sampleRate != originalRate)
    {
      region.sampleStartOffset = (uint16_t)std::clamp<int64_t>(
          std::llround(
              region.sampleStartOffset * region.sample.sampleRate / originalRate),
          0, 65535);
    }
  }

  // Remove regions that still have no sample data (failed to load)
  std::erase_if(destInstr.regions, [](const GigRegion& r) {
    return r.sample.data.empty() || r.sample.data[0].empty();
  });
}

/////////////////////////////
// GigaStudio / Gigasampler
/////////////////////////////

std::shared_ptr<GigFileInfo>
loadMetadata_gig(const QString& filePath, int instrumentIndex)
{
  auto riff = std::make_unique<RIFF::File>(filePath.toStdString());
  auto gigFile = std::make_unique<gig::File>(riff.get());

  auto info = std::make_shared<GigFileInfo>();
  info->filePath = filePath.toStdString();

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
      region.velHigh = 127;

      // Decode the dimension zones this region sits in: velocity range,
      // round-robin/random alternation and release triggers
      for(unsigned int dim = 0; dim < rgn->Dimensions; dim++)
      {
        const auto dimType = rgn->pDimensionDefinitions[dim].dimension;
        if(dimType != gig::dimension_velocity && dimType != gig::dimension_roundrobin
           && dimType != gig::dimension_roundrobinkeyboard
           && dimType != gig::dimension_random
           && dimType != gig::dimension_releasetrigger)
          continue;

        {
          int bitsBelow = 0;
          for(unsigned int dd = 0; dd < dim; dd++)
            bitsBelow += rgn->pDimensionDefinitions[dd].bits;

          const int dimBits = rgn->pDimensionDefinitions[dim].bits;
          const int mask = (1 << dimBits) - 1;
          const int zone = (d >> bitsBelow) & mask;
          const int zones = std::max<int>(1, rgn->pDimensionDefinitions[dim].zones);

          if(dimType == gig::dimension_releasetrigger)
          {
            region.releaseTrigger = zone > 0;
            continue;
          }
          if(dimType == gig::dimension_roundrobin
             || dimType == gig::dimension_roundrobinkeyboard)
          {
            region.rrIndex = zone;
            region.selectionAlgo = 1;
            continue;
          }
          if(dimType == gig::dimension_random)
          {
            region.rrIndex = zone;
            region.selectionAlgo = 2;
            continue;
          }

          // Per-zone upper limit: gig3 stores it in DimensionUpperLimits,
          // gig2 in VelocityUpperLimit (0 there means a uniform 128/zones
          // split). Zones are contiguous: a zone's lower bound is the
          // previous zone's upper bound + 1.
          auto zoneUpperLimit = [&](const gig::DimensionRegion* dr, int z) -> int {
            if(dr)
            {
              if(dr->DimensionUpperLimits[dim] > 0)
                return dr->DimensionUpperLimits[dim];
              if(dr->VelocityUpperLimit > 0)
                return dr->VelocityUpperLimit;
            }
            return (128 / zones) * (z + 1) - 1;
          };

          region.velHigh = std::clamp(zoneUpperLimit(dimRgn, zone), 0, 127);
          if(zone > 0)
          {
            const uint32_t prevIdx = d - (1u << bitsBelow);
            const gig::DimensionRegion* prev
                = prevIdx < rgn->DimensionRegions ? rgn->pDimensionRegions[prevIdx]
                                                  : nullptr;
            region.velLow = std::clamp(zoneUpperLimit(prev, zone - 1) + 1, 0, 127);
          }
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
      if(rgn->KeyGroup > 0)
        region.chokeGroup = rgn->KeyGroup;

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

      sanitizeRegion(region);
      destInstr.regions.push_back(std::move(region));
    }

    rgn = gigInstr->GetNextRegion();
  }

  return info;
}

// Phase 2a: read all raw sample data with libgig (single-threaded, libgig is
// not thread-safe). The walk order must match loadMetadata_gig exactly.
bool collectRawBuffers_gig(
    const GigFileInfo& info, std::vector<RawSampleBuffer>& rawBuffers,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  auto riff = std::make_unique<RIFF::File>(info.filePath);
  auto gigFile = std::make_unique<gig::File>(riff.get());

  gig::Instrument* gigInstr = gigFile->GetInstrument(info.selectedInstrument);
  if(!gigInstr)
    return false;

  const auto& destInstr = info.instruments[info.selectedInstrument];

  int regionIdx = 0;
  gig::Region* rgn = gigInstr->GetFirstRegion();
  while(rgn)
  {
    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return false;

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
  return true;
}

/////////////////////////////
// DLS Level 1 / 2
/////////////////////////////

// DLS stores envelope times as 32 bit time cents (1200 * 65536 per doubling);
// INT32_MIN denotes "instantaneous".
double dlsTimeCentsToSeconds(int32_t tc)
{
  if(tc == INT32_MIN)
    return 0.;
  return std::pow(2.0, tc / (1200.0 * 65536.0));
}

// Gain is stored as 32 bit fixed point relative gain in dB;
// same conversion libgig applies for gig::DimensionRegion::SampleAttenuation.
double dlsGainToLinear(int32_t gain)
{
  return std::pow(10.0, -gain / (20.0 * 655360.0));
}

void applyDlsArticulations(DLS::Articulator& art, GigRegion& region)
{
  for(auto* a = art.GetFirstArticulation(); a; a = art.GetNextArticulation())
  {
    for(uint32_t i = 0; i < a->Connections; i++)
    {
      const auto& c = a->pConnections[i];
      // Only plain destination assignments, no modulation routings
      if(c.Source != DLS::conn_src_none || c.Control != DLS::conn_src_none)
        continue;

      const auto scale = (int32_t)c.Scale;
      switch(c.Destination)
      {
        case DLS::conn_dst_eg1_attacktime:
          region.eg1Attack = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_decaytime:
          region.eg1Decay = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_releasetime:
          region.eg1Release = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_sustainlevel:
          // 16.16 fixed point percentage
          region.eg1Sustain = std::clamp(scale / 65536.0 / 100.0, 0.0, 1.0);
          break;
        case DLS::conn_dst_pan:
          // 16.16 fixed point percentage, -50% .. +50%
          region.pan = (int8_t)std::clamp<int>(
              (int)std::lround(scale / 65536.0 / 100.0 * 127.0), -64, 63);
          break;
        default:
          break;
      }
    }
  }
}

std::shared_ptr<GigFileInfo>
loadMetadata_dls(const QString& filePath, int instrumentIndex)
{
  auto riff = std::make_unique<RIFF::File>(filePath.toStdString());
  auto dlsFile = std::make_unique<DLS::File>(riff.get());

  auto info = std::make_shared<GigFileInfo>();
  info->filePath = filePath.toStdString();

  if(dlsFile->pInfo)
    info->name = dlsFile->pInfo->Name;
  if(info->name.empty())
    info->name = QFileInfo(filePath).baseName().toStdString();

  for(auto* in = dlsFile->GetFirstInstrument(); in; in = dlsFile->GetNextInstrument())
  {
    GigInstrument instr;
    if(in->pInfo)
      instr.name = in->pInfo->Name;
    info->instruments.push_back(std::move(instr));
  }

  if(info->instruments.empty())
    return {};

  if(instrumentIndex < 0 || instrumentIndex >= (int)info->instruments.size())
    instrumentIndex = 0;
  info->selectedInstrument = instrumentIndex;

  DLS::Instrument* dlsInstr = dlsFile->GetFirstInstrument();
  for(int i = 0; i < instrumentIndex && dlsInstr; i++)
    dlsInstr = dlsFile->GetNextInstrument();
  if(!dlsInstr)
    return {};

  auto& destInstr = info->instruments[instrumentIndex];

  for(auto* rgn = dlsInstr->GetFirstRegion(); rgn; rgn = dlsInstr->GetNextRegion())
  {
    auto* smp = rgn->GetSample();
    if(!smp || smp->SamplesTotal == 0)
      continue;

    GigRegion region;
    region.keyLow = (uint8_t)std::min<uint16_t>(rgn->KeyRange.low, 127);
    region.keyHigh = (uint8_t)std::min<uint16_t>(rgn->KeyRange.high, 127);

    // An unset velocity range (0, 0) means "all velocities"
    if(rgn->VelocityRange.high > 0)
    {
      region.velLow = (uint8_t)std::min<uint16_t>(rgn->VelocityRange.low, 127);
      region.velHigh = (uint8_t)std::min<uint16_t>(rgn->VelocityRange.high, 127);
    }

    region.sampleAttenuation = dlsGainToLinear(rgn->Gain);
    region.sample.sampleRate = smp->SamplesPerSecond;
    region.sample.midiUnityNote = std::min<uint32_t>(rgn->UnityNote, 127);
    region.sample.fineTune = rgn->FineTune;
    if(rgn->KeyGroup > 0)
      region.chokeGroup = rgn->KeyGroup;

    if(rgn->SampleLoops > 0 && rgn->pSampleLoops)
    {
      const auto& loop = rgn->pSampleLoops[0];
      if(loop.LoopLength > 0)
      {
        region.sample.hasLoop = true;
        region.sample.loopStart = loop.LoopStart;
        region.sample.loopEnd = loop.LoopStart + loop.LoopLength;
        region.sample.loopType = (int)loop.LoopType;
      }
    }

    // EG1 / pan from the articulation connection blocks; instrument-level
    // articulations first, then region-level ones override.
    applyDlsArticulations(*dlsInstr, region);
    applyDlsArticulations(*rgn, region);

    sanitizeRegion(region);
    destInstr.regions.push_back(std::move(region));
  }

  return info;
}

// Walk order must match loadMetadata_dls exactly.
bool collectRawBuffers_dls(
    const GigFileInfo& info, std::vector<RawSampleBuffer>& rawBuffers,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  auto riff = std::make_unique<RIFF::File>(info.filePath);
  auto dlsFile = std::make_unique<DLS::File>(riff.get());

  DLS::Instrument* dlsInstr = dlsFile->GetFirstInstrument();
  for(int i = 0; i < info.selectedInstrument && dlsInstr; i++)
    dlsInstr = dlsFile->GetNextInstrument();
  if(!dlsInstr)
    return false;

  const auto& destInstr = info.instruments[info.selectedInstrument];

  int regionIdx = 0;
  for(auto* rgn = dlsInstr->GetFirstRegion(); rgn; rgn = dlsInstr->GetNextRegion())
  {
    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return false;

    auto* smp = rgn->GetSample();
    if(!smp || smp->SamplesTotal == 0)
      continue;

    if(regionIdx >= (int)destInstr.regions.size())
      break;

    if(void* p = smp->LoadSampleData())
    {
      const int64_t bytes = (int64_t)smp->GetSize() * smp->FrameSize;
      if(bytes > 0)
      {
        RawSampleBuffer raw;
        raw.data.resize(bytes);
        std::memcpy(raw.data.data(), p, bytes);
        raw.channels = smp->Channels;
        raw.bitDepth = smp->BitDepth;
        raw.totalSamples = smp->SamplesTotal;
        raw.regionIndex = regionIdx;
        rawBuffers.push_back(std::move(raw));
      }
      smp->ReleaseSampleData();
    }

    regionIdx++;
  }
  return true;
}

/////////////////////////////
// SoundFont 2
/////////////////////////////

// sf2 regions use -1 ("NONE") for unset range bounds
int sf2RangeValue(int v, int fallback)
{
  return v < 0 || v > 127 ? fallback : v;
}

// The executor maps cutoff 0-127 to 20 Hz - 20 kHz as 20 * 1000^(v/127)
uint8_t frequencyToVcfCutoff(double freq)
{
  freq = std::clamp(freq, 20.0, 20000.0);
  return (uint8_t)std::clamp(
      (int)std::lround(127.0 * std::log(freq / 20.0) / std::log(1000.0)), 0, 127);
}

std::shared_ptr<GigFileInfo>
loadMetadata_sf2(const QString& filePath, int instrumentIndex)
{
  auto riff = std::make_unique<RIFF::File>(filePath.toStdString());
  auto sfFile = std::make_unique<sf2::File>(riff.get());

  auto info = std::make_shared<GigFileInfo>();
  info->filePath = filePath.toStdString();

  if(sfFile->pInfo)
    info->name = sfFile->pInfo->BankName;
  if(info->name.empty())
    info->name = QFileInfo(filePath).baseName().toStdString();

  // Each SoundFont preset maps to one instrument entry
  const int presets = sfFile->GetPresetCount();
  for(int i = 0; i < presets; i++)
  {
    GigInstrument instr;
    if(auto* p = sfFile->GetPreset(i))
      instr.name = p->Name;
    info->instruments.push_back(std::move(instr));
  }

  if(info->instruments.empty())
    return {};

  if(instrumentIndex < 0 || instrumentIndex >= (int)info->instruments.size())
    instrumentIndex = 0;
  info->selectedInstrument = instrumentIndex;

  sf2::Preset* preset = sfFile->GetPreset(instrumentIndex);
  if(!preset)
    return {};

  auto& destInstr = info->instruments[instrumentIndex];

  // Preset zones point to instruments, whose zones point to samples;
  // generator values combine across both levels (libgig's Get*(pPresetRegion)
  // accessors implement the layering).
  for(int pr = 0; pr < preset->GetRegionCount(); pr++)
  {
    sf2::Region* pz = preset->GetRegion(pr);
    if(!pz || !pz->pInstrument)
      continue;

    const int pKeyLow = sf2RangeValue(pz->loKey, 0);
    const int pKeyHigh = sf2RangeValue(pz->hiKey, 127);
    const int pVelLow = sf2RangeValue(pz->minVel, 0);
    const int pVelHigh = sf2RangeValue(pz->maxVel, 127);

    for(int ir = 0; ir < pz->pInstrument->GetRegionCount(); ir++)
    {
      sf2::Region* iz = pz->pInstrument->GetRegion(ir);
      if(!iz || !iz->pSample)
        continue;
      auto* smp = iz->pSample;
      if(smp->GetTotalFrameCount() <= 0)
        continue;

      const int keyLow = std::max(sf2RangeValue(iz->loKey, 0), pKeyLow);
      const int keyHigh = std::min(sf2RangeValue(iz->hiKey, 127), pKeyHigh);
      const int velLow = std::max(sf2RangeValue(iz->minVel, 0), pVelLow);
      const int velHigh = std::min(sf2RangeValue(iz->maxVel, 127), pVelHigh);
      if(keyLow > keyHigh || velLow > velHigh)
        continue;

      GigRegion region;
      region.keyLow = keyLow;
      region.keyHigh = keyHigh;
      region.velLow = velLow;
      region.velHigh = velHigh;

      region.pitchTrack = !smp->IsUnpitched();
      // initialAttenuation is in centibels of attenuation (0..1440)
      region.sampleAttenuation = std::pow(
          10.0, -std::clamp(iz->GetInitialAttenuation(pz), 0, 1440) / 200.0);
      region.sample.midiUnityNote = (uint32_t)std::clamp(
          iz->GetUnityNote() - iz->GetCoarseTune(pz), 0, 127);
      region.sample.fineTune = iz->GetFineTune(pz);
      region.pan = (int8_t)std::clamp(iz->GetPan(pz), -64, 63);
      region.sampleStartOffset = (uint16_t)std::clamp(
          iz->startAddrsOffset + 32768 * iz->startAddrsCoarseOffset, 0, 65535);

      // EG1: hold folded into the decay stage (our envelope has no hold)
      region.eg1Attack = iz->GetEG1Attack(pz);
      region.eg1Decay = iz->GetEG1Hold(pz) + iz->GetEG1Decay(pz);
      // Sustain is an attenuation in centibels
      const double sustainCb = std::clamp(iz->GetEG1Sustain(pz), 0, 1440);
      region.eg1Sustain = std::pow(10.0, -sustainCb / 200.0);
      region.eg1Release = iz->GetEG1Release(pz);

      // Filter: 13500 absolute cents is the "fully open" default
      const int fc = iz->GetInitialFilterFc(pz);
      if(fc > 0 && fc < 13500)
      {
        region.vcfEnabled = true;
        region.vcfCutoff = frequencyToVcfCutoff(8.176 * std::pow(2.0, fc / 1200.0));
        // Q in centibels -> the executor maps resonance 0-127 to Q 1-10
        const double q = std::pow(10.0, std::clamp(iz->GetInitialFilterQ(pz), 0, 960) / 200.0);
        region.vcfResonance
            = (uint8_t)std::clamp((int)std::lround((q - 1.0) * 127.0 / 9.0), 0, 127);
      }

      region.sample.sampleRate = smp->SampleRate;
      if(iz->HasLoop)
      {
        region.sample.hasLoop = true;
        region.sample.loopStart = iz->LoopStart;
        region.sample.loopEnd = iz->LoopEnd;
      }
      if(iz->exclusiveClass > 0)
        region.chokeGroup = (int)iz->exclusiveClass;

      sanitizeRegion(region);
      destInstr.regions.push_back(std::move(region));
    }
  }

  return info;
}

// Walk order must match loadMetadata_sf2 exactly.
bool collectRawBuffers_sf2(
    const GigFileInfo& info, std::vector<RawSampleBuffer>& rawBuffers,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  auto riff = std::make_unique<RIFF::File>(info.filePath);
  auto sfFile = std::make_unique<sf2::File>(riff.get());

  sf2::Preset* preset = sfFile->GetPreset(info.selectedInstrument);
  if(!preset)
    return false;

  const auto& destInstr = info.instruments[info.selectedInstrument];

  int regionIdx = 0;
  for(int pr = 0; pr < preset->GetRegionCount(); pr++)
  {
    sf2::Region* pz = preset->GetRegion(pr);
    if(!pz || !pz->pInstrument)
      continue;

    const int pKeyLow = sf2RangeValue(pz->loKey, 0);
    const int pKeyHigh = sf2RangeValue(pz->hiKey, 127);
    const int pVelLow = sf2RangeValue(pz->minVel, 0);
    const int pVelHigh = sf2RangeValue(pz->maxVel, 127);

    for(int ir = 0; ir < pz->pInstrument->GetRegionCount(); ir++)
    {
      if(cancelled && cancelled->load(std::memory_order_relaxed))
        return false;

      sf2::Region* iz = pz->pInstrument->GetRegion(ir);
      if(!iz || !iz->pSample)
        continue;
      auto* smp = iz->pSample;
      if(smp->GetTotalFrameCount() <= 0)
        continue;

      const int keyLow = std::max(sf2RangeValue(iz->loKey, 0), pKeyLow);
      const int keyHigh = std::min(sf2RangeValue(iz->hiKey, 127), pKeyHigh);
      const int velLow = std::max(sf2RangeValue(iz->minVel, 0), pVelLow);
      const int velHigh = std::min(sf2RangeValue(iz->maxVel, 127), pVelHigh);
      if(keyLow > keyHigh || velLow > velHigh)
        continue;

      if(regionIdx >= (int)destInstr.regions.size())
        break;

      sf2::Sample::buffer_t buf = smp->LoadSampleData();
      if(buf.pStart && buf.Size > 0)
      {
        RawSampleBuffer raw;
        raw.data.resize(buf.Size);
        std::memcpy(raw.data.data(), buf.pStart, buf.Size);
        raw.channels = smp->GetChannelCount();
        raw.bitDepth
            = smp->GetChannelCount() > 0
                  ? 8 * (smp->GetFrameSize() / smp->GetChannelCount())
                  : 16;
        raw.totalSamples = smp->GetTotalFrameCount();
        raw.regionIndex = regionIdx;
        rawBuffers.push_back(std::move(raw));
      }
      smp->ReleaseSampleData();

      regionIdx++;
    }
  }
  return true;
}

/////////////////////////////
// Hydrogen drumkits
/////////////////////////////

// The executor maps cutoff 0-127 to 20 Hz - 20 kHz as 20 * 1000^(v/127);
// Hydrogen's filter cutoff is a linear 0-1 factor of 20 kHz.
uint8_t hydrogenCutoffToVcf(double cutoff01)
{
  return frequencyToVcfCutoff(std::clamp(cutoff01 * 20000., 20., 20000.));
}

// Numeric XML element with a default: missing elements and non-finite or
// unparseable values (hand-edited kits do contain "nan"s) yield the default.
double xmlNumber(const QDomElement& parent, const char* name, double def)
{
  const auto e = parent.firstChildElement(name);
  if(e.isNull())
    return def;
  bool ok{};
  const double v = e.text().toDouble(&ok);
  return ok && std::isfinite(v) ? v : def;
}

// Parses drumkit.xml into the neutral instrument model; every velocity layer
// of every drum becomes one single-note, one-shot region. No audio is read.
std::shared_ptr<GigFileInfo>
loadMetadata_hydrogen(const QString& filePath, int instrumentIndex)
{
  QFile file(filePath);
  if(!file.open(QIODevice::ReadOnly))
    return {};

  QDomDocument doc;
  if(!doc.setContent(&file))
    return {};
  file.close();

  QDomElement root = doc.documentElement();
  if(root.tagName() != "drumkit_info")
    return {};

  const auto dir = QFileInfo{filePath}.dir();

  auto info = std::make_shared<GigFileInfo>();
  info->filePath = filePath.toStdString();
  info->selectedInstrument = 0;
  info->name = root.firstChildElement("name").text().toStdString();
  if(info->name.empty())
    info->name = dir.dirName().toStdString();

  GigInstrument kit;
  kit.name = info->name;

  // Hydrogen stores ADSR stage lengths as frame counts at 44.1 kHz
  constexpr double hydrogenFramesToSeconds = 1.0 / 44100.0;

  int base_midi_note = 36; // for instruments that do not specify a note
  for(QDomElement inst
      = root.firstChildElement("instrumentList").firstChildElement("instrument");
      !inst.isNull(); inst = inst.nextSiblingElement("instrument"))
  {
    const auto name = inst.firstChildElement("name").text();

    int midi_note = base_midi_note;
    if(const auto midiOutNote = inst.firstChildElement("midiOutNote");
       !midiOutNote.isNull())
    {
      midi_note = midiOutNote.text().toInt();
    }
    else
    {
      // Old kits: some instrument names start with their note number
      auto num = name;
      for(int i = 0; i < num.size(); i++)
      {
        if(num[i].isDigit())
          continue;
        num = num.mid(0, i);
        break;
      }
      bool ok{};
      const int n = num.toInt(&ok);
      if(ok && n >= 0 && n <= 127)
        midi_note = n;
    }
    midi_note = std::clamp(midi_note, 0, 127);
    base_midi_note = midi_note + 1;

    GigRegion base;
    base.keyLow = base.keyHigh = midi_note;
    base.pitchTrack = false;

    // isStopNote (a "stop note" makes note-off cut the sound) is the inverse
    // of one-shot playback
    base.oneShot = inst.firstChildElement("isStopNote").text() != "true";
    base.muted = inst.firstChildElement("isMuted").text() == "true";
    if(const auto applyVel = inst.firstChildElement("applyVelocity");
       !applyVel.isNull())
      base.applyVelocity = applyVel.text() == "true";

    // Hydrogen >= 1.1: how the layer within a velocity range is picked.
    // In format v2 the element moved inside <instrumentComponent>.
    const auto algoOf = [](const QDomElement& e) {
      const auto t = e.text();
      return t == QStringLiteral("ROUND_ROBIN") ? 1
             : t == QStringLiteral("RANDOM")    ? 2
                                                : 0;
    };
    if(const auto algo = inst.firstChildElement("sampleSelectionAlgo"); !algo.isNull())
      base.selectionAlgo = algoOf(algo);
    else if(const auto c = inst.firstChildElement("instrumentComponent"); !c.isNull())
      if(const auto algo2 = c.firstChildElement("sampleSelectionAlgo"); !algo2.isNull())
        base.selectionAlgo = algoOf(algo2);

    const double volume = xmlNumber(inst, "volume", 1.0);
    const double instGain = xmlNumber(inst, "gain", 1.0);
    const double instPitch = xmlNumber(inst, "pitchOffset", 0.0);
    base.randomPitch = xmlNumber(inst, "randomPitchFactor", 0.0);

    if(const int muteGroup = (int)xmlNumber(inst, "muteGroup", -1.0); muteGroup >= 0)
      base.chokeGroup = muteGroup;

    // Hydrogen <= 1.1 stores pan as two 0-1 attenuations, >= 1.2 as a single
    // -1..1 <pan> element
    if(!inst.firstChildElement("pan").isNull())
    {
      base.pan = (int8_t)std::clamp(
          (int)std::lround(xmlNumber(inst, "pan", 0.0) * 63.), -64, 63);
    }
    else
    {
      const double pan_L = xmlNumber(inst, "pan_L", 1.0);
      const double pan_R = xmlNumber(inst, "pan_R", 1.0);
      base.pan = (int8_t)std::clamp((int)std::lround((pan_R - pan_L) * 63.), -64, 63);
    }

    if(!inst.firstChildElement("filterActive").isNull())
    {
      base.vcfEnabled = inst.firstChildElement("filterActive").text() == "true";
      base.vcfCutoff
          = hydrogenCutoffToVcf(std::clamp(xmlNumber(inst, "filterCutoff", 1.0), 0., 1.));
      const double res = std::clamp(xmlNumber(inst, "filterResonance", 0.0), 0., 1.);
      // Hydrogen maps resonance 0-1 to Q 0.1-10.1; the executor maps 0-127 to Q 1-10
      base.vcfResonance
          = (uint8_t)std::clamp((int)std::lround(res * 10. * 127. / 9.), 0, 127);
    }

    if(!inst.firstChildElement("Attack").isNull())
    {
      base.eg1Attack = xmlNumber(inst, "Attack", 0.0) * hydrogenFramesToSeconds;
      base.eg1Decay = xmlNumber(inst, "Decay", 0.0) * hydrogenFramesToSeconds;
      base.eg1Sustain = xmlNumber(inst, "Sustain", 1.0);
      base.eg1Release = xmlNumber(inst, "Release", 1000.0) * hydrogenFramesToSeconds;
    }

    // Layers appear in three generations of the schema: bare <filename>,
    // <layer> children, or <instrumentComponent><layer>.
    auto addLayer = [&](const QDomElement& layerElem) {
      const auto filename = layerElem.firstChildElement("filename").text();
      if(filename.isEmpty() || !dir.exists(filename))
        return;

      GigRegion region = base;
      region.sample.sourceFile = dir.absoluteFilePath(filename).toStdString();

      if(!layerElem.firstChildElement("min").isNull())
      {
        const double minV = xmlNumber(layerElem, "min", 0.0);
        const double maxV = xmlNumber(layerElem, "max", 1.0);
        // Hydrogen layer ranges are half-open [min, max): the boundary
        // velocity belongs to the upper layer, otherwise adjacent layers
        // would both trigger on it
        region.velLow
            = (uint8_t)std::clamp((int)std::lround(minV * 127.), 0, 127);
        region.velHigh = maxV >= 1.0
                             ? uint8_t(127)
                             : (uint8_t)std::clamp(
                                   (int)std::lround(maxV * 127.) - 1, 0, 127);
        if(region.velLow > region.velHigh)
          std::swap(region.velLow, region.velHigh);
      }

      region.sampleAttenuation
          = volume * instGain * xmlNumber(layerElem, "gain", 1.0);
      region.pitchOffset = instPitch + xmlNumber(layerElem, "pitch", 0.0);

      sanitizeRegion(region);
      kit.regions.push_back(std::move(region));
    };

    const auto layers = inst.firstChildElement("layer");
    const auto component = inst.firstChildElement("instrumentComponent");
    if(layers.isNull() && component.isNull())
    {
      // Oldest schema: a single <filename> directly on the instrument
      addLayer(inst);
    }
    else if(!layers.isNull())
    {
      for(QDomElement l = layers; !l.isNull(); l = l.nextSiblingElement("layer"))
        addLayer(l);
    }
    else
    {
      for(QDomElement l = component.firstChildElement("layer"); !l.isNull();
          l = l.nextSiblingElement("layer"))
        addLayer(l);
    }
  }

  if(kit.regions.empty())
    return {};

  info->instruments.push_back(std::move(kit));
  return info;
}

// Phase 2 for drumkits: decode each layer's audio file at the target rate
bool loadSamples_hydrogen(
    GigFileInfo& info, int targetRate,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  auto& destInstr = info.instruments[info.selectedInstrument];
  for(auto& region : destInstr.regions)
  {
    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return false;
    if(region.sample.sourceFile.empty())
      continue;

    try
    {
      auto dec = Media::AudioDecoder::decode_synchronous(
          QString::fromStdString(region.sample.sourceFile), targetRate);
      if(dec)
      {
        region.sample.data = std::move(dec->second);
        region.sample.sampleRate = targetRate;

        // Float audio files can legitimately contain NaN / inf samples; they
        // must never reach the audio thread (a NaN sticks in the filters)
        for(auto& channel : region.sample.data)
          for(auto& sample : channel)
            if(!std::isfinite(sample))
              sample = 0.;
      }
    }
    catch(...)
    {
      // Undecodable layers are pruned with the other empty regions
    }
  }
  return true;
}

}

/////////////////////////////
// Public entry points
/////////////////////////////

ParsedInstrumentPath parseInstrumentPath(const QString& data)
{
  if(const auto sep = data.lastIndexOf('|'); sep > 0)
  {
    bool ok{};
    const int idx = QStringView{data}.mid(sep + 1).toInt(&ok);
    // '|' is legal in POSIX file names: only treat the suffix as an
    // instrument index when the full string is not itself an existing file
    if(ok && idx >= 0 && !QFileInfo::exists(data))
      return {data.left(sep), idx};
  }
  return {data, 0};
}

std::vector<std::string> listInstruments(const QString& filePath)
{
  std::vector<std::string> names;

  const auto format = formatForPath(filePath);
  if(format == SampleFileFormat::Hydrogen)
  {
    if(auto info = loadMetadata_hydrogen(filePath, 0))
      names.push_back(info->name);
    return names;
  }

  try
  {
    auto riff = std::make_unique<RIFF::File>(filePath.toStdString());
    switch(format)
    {
      case SampleFileFormat::Dls: {
        auto f = std::make_unique<DLS::File>(riff.get());
        for(auto* in = f->GetFirstInstrument(); in; in = f->GetNextInstrument())
          names.push_back(in->pInfo ? in->pInfo->Name : std::string{});
        break;
      }
      case SampleFileFormat::Sf2: {
        auto f = std::make_unique<sf2::File>(riff.get());
        for(int i = 0, n = f->GetPresetCount(); i < n; i++)
        {
          auto* p = f->GetPreset(i);
          names.push_back(p ? p->Name : std::string{});
        }
        break;
      }
      case SampleFileFormat::Gig: {
        auto f = std::make_unique<gig::File>(riff.get());
        for(auto* in = f->GetFirstInstrument(); in; in = f->GetNextInstrument())
          names.push_back(in->pInfo ? in->pInfo->Name : std::string{});
        break;
      }
      case SampleFileFormat::Hydrogen:
        // handled before the RIFF file is opened
        break;
    }
  }
  catch(...)
  {
    return {};
  }
  return names;
}

QString formatName(const QString& filePath)
{
  switch(formatForPath(filePath))
  {
    case SampleFileFormat::Dls:
      return QStringLiteral("DLS");
    case SampleFileFormat::Sf2:
      return QStringLiteral("SF2");
    case SampleFileFormat::Hydrogen:
      return QStringLiteral("Drumkit");
    case SampleFileFormat::Gig:
      break;
  }
  return QStringLiteral("GIG");
}

std::shared_ptr<GigFileInfo>
loadGigFileMetadata(const QString& filePath, int instrumentIndex)
{
  try
  {
    switch(formatForPath(filePath))
    {
      case SampleFileFormat::Dls:
        return loadMetadata_dls(filePath, instrumentIndex);
      case SampleFileFormat::Sf2:
        return loadMetadata_sf2(filePath, instrumentIndex);
      case SampleFileFormat::Hydrogen:
        return loadMetadata_hydrogen(filePath, instrumentIndex);
      case SampleFileFormat::Gig:
        return loadMetadata_gig(filePath, instrumentIndex);
    }
    return {};
  }
  catch(RIFF::Exception& e)
  {
    qWarning() << "libgig metadata error:" << e.Message.c_str();
    return {};
  }
  catch(...)
  {
    qWarning() << "Unknown error parsing instrument metadata:" << filePath;
    return {};
  }
}

std::shared_ptr<GigFileInfo> loadGigFileSamples(
    const std::shared_ptr<GigFileInfo>& metadata,
    int targetRate,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  if(!metadata)
    return {};

  try
  {
    // Make a deep copy of the metadata so we can fill in sample data
    // without racing with the GUI thread
    auto info = std::make_shared<GigFileInfo>(*metadata);

    const int instrIdx = info->selectedInstrument;
    if(instrIdx < 0 || instrIdx >= (int)info->instruments.size())
      return {};

    // Phase 2a: read all raw sample data (single-threaded, libgig is not
    // thread-safe)
    std::vector<RawSampleBuffer> rawBuffers;
    rawBuffers.reserve(info->instruments[instrIdx].regions.size());

    bool ok{};
    switch(formatForPath(QString::fromStdString(info->filePath)))
    {
      case SampleFileFormat::Dls:
        ok = collectRawBuffers_dls(*info, rawBuffers, cancelled);
        break;
      case SampleFileFormat::Sf2:
        ok = collectRawBuffers_sf2(*info, rawBuffers, cancelled);
        break;
      case SampleFileFormat::Hydrogen:
        // Decodes external audio files directly; nothing to convert, but the
        // shared pruning of empty regions below still applies
        ok = loadSamples_hydrogen(*info, targetRate, cancelled);
        break;
      case SampleFileFormat::Gig:
        ok = collectRawBuffers_gig(*info, rawBuffers, cancelled);
        break;
    }
    if(!ok)
      return {};

    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return {};

    // Phase 2b: convert and resample each sample
    bool wasCancelled{};
    convertAndPrune(
        rawBuffers, info->instruments[instrIdx], targetRate, cancelled,
        wasCancelled);
    if(wasCancelled)
      return {};

    return info;
  }
  catch(RIFF::Exception& e)
  {
    qWarning() << "libgig sample loading error:" << e.Message.c_str();
    return {};
  }
  catch(...)
  {
    qWarning() << "Unknown error loading samples";
    return {};
  }
}

}
