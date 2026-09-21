#include "GigLoader.hpp"

#include <Media/AudioDecoder.hpp>
#include <Media/MediaFileHandle.hpp>

#include <QDir>
#include <QDirIterator>
#include <QXmlStreamReader>
#include <QHash>
#include <QFile>
#include <QFileInfo>

#include <DLS.h>
#include <Korg.h>
#include <SF.h>
#include <gig.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace Deuterium::Gig
{
namespace
{

enum class SampleFileFormat
{
  Gig,
  Dls,
  Sf2,
  Hydrogen,
  Korg,
  AudioFile
};

bool isPlainAudioExtension(const QString& ext)
{
  return ext == "wav" || ext == "flac" || ext == "ogg" || ext == "aiff"
         || ext == "aif" || ext == "mp3";
}

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
    if(ext == "kmp")
      return SampleFileFormat::Korg;
    if(isPlainAudioExtension(ext))
      return SampleFileFormat::AudioFile;
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

/////////////////////////////
// Cache identity and counters
/////////////////////////////

struct LoaderCounters
{
  std::atomic<uint64_t> bankParses{};
  std::atomic<uint64_t> bankHits{};
  std::atomic<uint64_t> audioDecodes{};
  std::atomic<uint64_t> audioHits{};
  std::atomic<uint64_t> drwavDecodes{};
  std::atomic<uint64_t> libavDecodes{};
  std::atomic<uint64_t> parallelConversions{};
};
LoaderCounters& counters()
{
  static LoaderCounters c;
  return c;
}

// Identity of a file on disk: everything cached here is keyed on the path
// plus this, so a bank or a sample edited under score is re-read.
struct FileStamp
{
  int64_t size{-1};
  int64_t mtime{-1};
  bool valid() const noexcept { return size >= 0; }
  friend bool operator==(const FileStamp&, const FileStamp&) noexcept = default;
};

FileStamp stampOf(const QString& path)
{
#if defined(_WIN32)
  const std::filesystem::path p{path.toStdWString()};
#else
  const std::filesystem::path p{QFile::encodeName(path).toStdString()};
#endif
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  if(ec)
    return {};
  const auto mtime = std::filesystem::last_write_time(p, ec);
  if(ec)
    return {};
  return {(int64_t)size, (int64_t)mtime.time_since_epoch().count()};
}

/////////////////////////////
// Parsed bank cache
/////////////////////////////

// One parsed bank file. The format-specific objects all borrow the RIFF
// file, so they must be declared after it (destruction runs in reverse).
struct ParsedBank
{
  std::string path;
  FileStamp stamp;
  SampleFileFormat format{};
  std::unique_ptr<RIFF::File> riff;
  std::unique_ptr<gig::File> gig;
  std::unique_ptr<DLS::File> dls;
  std::unique_ptr<sf2::File> sf2;
};

struct BankCache
{
  std::mutex mutex;
  std::deque<std::unique_ptr<ParsedBank>> entries; // most recent first
};
BankCache& bankCache()
{
  static BankCache c;
  return c;
}

// Two is enough for the operation this exists for -- switching instrument
// inside one bank, where the metadata phase, the sample phase and the next
// switch all want the same file -- while bounding how much parsed bank
// metadata stays resident.
constexpr std::size_t kMaxCachedBanks = 2;

// Keeps the libgig lock for its lifetime: the bank it points to may only be
// touched while it is alive.
struct BankHandle
{
  std::unique_lock<std::mutex> lock;
  std::unique_ptr<ParsedBank> owned; // set when the bank is not cached
  ParsedBank* bank{};

  ParsedBank* operator->() const noexcept { return bank; }
};

// libgig carries process-global mutable state -- gig::Sample's shared
// decompression buffer and the non-atomic instance counters that own it --
// so every libgig object, from parsing to sample reads to destruction,
// lives under this one lock. Serialising that is affordable precisely
// because the cache removes the repeated parses: constructing a gig::File
// scans the compressed data of every sample in the file, which is what made
// switching instrument inside a bank slow.
//
// `keep` is false for the library scanner: it walks thousands of files once
// and must not evict the bank the user is playing.
BankHandle openBank(const QString& path, SampleFileFormat format, bool keep)
{
  auto& cache = bankCache();
  BankHandle h{std::unique_lock{cache.mutex}};

  const auto p = path.toStdString();
  const auto stamp = stampOf(path);
  auto& entries = cache.entries;

  for(auto it = entries.begin(); it != entries.end(); ++it)
  {
    if((*it)->path != p)
      continue;
    if((*it)->stamp == stamp && (*it)->format == format && stamp.valid())
    {
      auto hit = std::move(*it);
      entries.erase(it);
      h.bank = hit.get();
      entries.push_front(std::move(hit));
      counters().bankHits.fetch_add(1, std::memory_order_relaxed);
      return h;
    }
    entries.erase(it); // the file changed underneath us
    break;
  }

  auto e = std::make_unique<ParsedBank>();
  e->path = p;
  e->stamp = stamp;
  e->format = format;
  e->riff = std::make_unique<RIFF::File>(p);
  switch(format)
  {
    case SampleFileFormat::Dls:
      e->dls = std::make_unique<DLS::File>(e->riff.get());
      break;
    case SampleFileFormat::Sf2:
      e->sf2 = std::make_unique<sf2::File>(e->riff.get());
      break;
    case SampleFileFormat::Gig:
      e->gig = std::make_unique<gig::File>(e->riff.get());
      break;
    default:
      break;
  }
  counters().bankParses.fetch_add(1, std::memory_order_relaxed);

  h.bank = e.get();
  if(keep && stamp.valid())
  {
    entries.push_front(std::move(e));
    while(entries.size() > kMaxCachedBanks)
      entries.pop_back();
  }
  else
  {
    h.owned = std::move(e);
  }
  return h;
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
  // The engine subtracts this from the note and exponentiates the result:
  // out-of-range values from corrupt files would blow up the pitch ratio
  if(r.sample.midiUnityNote > 127)
    r.sample.midiUnityNote = 60;
}

// Raw PCM read back from a file, one per unique source sample; regionIndices
// lists every region referencing it (the decoded audio is shared)
struct RawSampleBuffer
{
  std::vector<uint8_t> data;
  int channels{};
  int bitDepth{};
  bool signed8{false}; // 8-bit: RIFF formats are unsigned, Korg is signed
  int takeChannel{-1}; // >= 0: collapse a stereo frame to this channel
  int64_t totalSamples{};
  uint32_t sourceRate{44100};
  std::vector<int> regionIndices;
};

// Precomputes the alternation groups: regions sharing the exact same key and
// velocity zone are alternatives of one another (round-robin/random). Doing
// this at load time keeps the audio thread's note-on scan linear.
void assignAlternationGroups(GigInstrument& instr)
{
  std::unordered_map<uint32_t, std::vector<int>> zones;
  for(std::size_t i = 0; i < instr.regions.size(); i++)
  {
    auto& r = instr.regions[i];
    r.altGroup = -1;
    r.altIndex = 0;
    r.altCount = 1;
    if(r.muted || r.releaseTrigger)
      continue;
    const uint32_t key = (uint32_t(r.keyLow) << 24) | (uint32_t(r.keyHigh) << 16)
                         | (uint32_t(r.velLow) << 8) | uint32_t(r.velHigh);
    zones[key].push_back((int)i);
  }

  int group = 0;
  for(auto& [key, members] : zones)
  {
    if(members.size() <= 1)
      continue;
    std::stable_sort(members.begin(), members.end(), [&](int a, int b) {
      return instr.regions[a].rrIndex < instr.regions[b].rrIndex;
    });
    const int count = std::min<int>(members.size(), 255);
    for(int n = 0; n < count; n++)
    {
      auto& r = instr.regions[members[n]];
      r.altGroup = group;
      r.altIndex = (uint8_t)n;
      r.altCount = (uint8_t)count;
    }
    group++;
  }
}

// Converts raw PCM into a shared double array, resampled to targetRate.
// `ratio` reports the resampling factor actually applied (1 = none) so the
// per-region loop points and offsets can be rescaled by the caller.
struct ConvertedSample
{
  std::shared_ptr<ossia::audio_array> data;
  double ratio{1.0};
  uint32_t rate{44100};
};
//! Polyphase windowed-sinc table, cached per thread.
//!
//! It depends only on the cutoff, which follows the resampling ratio -- the
//! same for every sample of a bank at a given engine rate. Rebuilding it per
//! sample cost around 50k transcendental calls each time, which dominated the
//! load of a large kit.
static const std::vector<double>&
resamplingKernel(double cutoff, int taps, int half, int phases)
{
  thread_local std::vector<double> kernel;
  thread_local double cachedCutoff = -1.;
  thread_local int cachedTaps = 0;
  thread_local int cachedPhases = 0;

  if(cutoff == cachedCutoff && taps == cachedTaps && phases == cachedPhases)
    return kernel;

  kernel.assign((phases + 1) * taps, 0.);
  for(int ph = 0; ph <= phases; ph++)
  {
    const double frac = (double)ph / phases;
    double sum = 0.;
    for(int t = 0; t < taps; t++)
    {
      const double dist = (t - half + 1) - frac; // tap offset from srcPos
      const double x = dist * cutoff;
      const double sinc = x == 0. ? 1. : std::sin(M_PI * x) / (M_PI * x);
      const double wArg = dist / half;
      const double w = std::abs(wArg) >= 1.
                           ? 0.
                           : 0.42 + 0.5 * std::cos(M_PI * wArg)
                                 + 0.08 * std::cos(2. * M_PI * wArg);
      kernel[ph * taps + t] = cutoff * sinc * w;
      sum += kernel[ph * taps + t];
    }
    // Unity DC gain per phase (kills fractional-position gain ripple)
    if(sum > 1e-9)
      for(int t = 0; t < taps; t++)
        kernel[ph * taps + t] /= sum;
  }

  cachedCutoff = cutoff;
  cachedTaps = taps;
  cachedPhases = phases;
  return kernel;
}

// Resamples decoded audio to targetRate in place-ish (the input is consumed).
// Shared by the bank formats and by the external audio file decoders.
ConvertedSample
resampleArray(ossia::audio_array&& input, uint32_t sourceRate, int targetRate)
{
  ConvertedSample result;
  const int outChannels = (int)input.size();
  const int64_t totalSamples = outChannels > 0 ? (int64_t)input[0].size() : 0;

  // Never trust the header sample rate: 0 would divide to an infinite ratio
  // below (with an UB float->int cast), and absurd values would make the
  // resampled allocation explode
  if(sourceRate < 4000 || sourceRate > 768000)
    sourceRate = (uint32_t)targetRate;
  result.rate = sourceRate;

  auto arr = std::make_shared<ossia::audio_array>(std::move(input));
  if(totalSamples > 0 && sourceRate != (uint32_t)targetRate && targetRate > 0)
  {
    const double ratio = (double)targetRate / (double)sourceRate;
    const int64_t newLen = (int64_t)(totalSamples * ratio);
    // The upper bound caps the resampled buffer at 2 GB per channel
    if(newLen > 0 && newLen < (int64_t(1) << 28))
    {
      // Windowed-sinc resampling through a polyphase kernel table:
      // flat passband and real alias rejection on downsampling, unlike
      // linear interpolation. Runs on the loader thread, never on the
      // audio thread.
      constexpr int taps = 32;
      constexpr int half = taps / 2;
      constexpr int phases = 512;
      const double cutoff = 0.90 * std::min(1.0, ratio); // rel. source Nyquist
      const std::vector<double>& kernel
          = resamplingKernel(cutoff, taps, half, phases);

      auto resampled = std::make_shared<ossia::audio_array>();
      resampled->resize(outChannels);
      for(int c = 0; c < outChannels; c++)
      {
        (*resampled)[c].resize(newLen);
        const auto& src = (*arr)[c];
        for(int64_t i = 0; i < newLen; i++)
        {
          const double srcPos = i / ratio;
          const int64_t idx = (int64_t)srcPos;
          const int ph
              = (int)std::lround((srcPos - idx) * phases); // 0..phases
          const double* k = &kernel[ph * taps];
          double acc = 0.;
          for(int t = 0; t < taps; t++)
          {
            const int64_t j = idx - half + 1 + t;
            if(j >= 0 && j < totalSamples)
              acc += src[j] * k[t];
          }
          (*resampled)[c][i] = acc;
        }
        // The source channel is dead once it is resampled: releasing it
        // here halves the peak footprint of a large bank
        (*arr)[c] = {};
      }
      arr = std::move(resampled);
      result.ratio = ratio;
      result.rate = (uint32_t)targetRate;
    }
  }

  result.data = std::move(arr);
  return result;
}

ConvertedSample convertRawSampleData(
    const void* rawData, int64_t rawSize, int channels, int bitDepth,
    int64_t totalSamples, uint32_t sourceRate, int targetRate,
    bool signed8 = false, int takeChannel = -1)
{
  ConvertedSample result;
  if(!rawData || rawSize <= 0 || totalSamples <= 0)
    return result;
  if(bitDepth != 8 && bitDepth != 16 && bitDepth != 24)
    return result;

  // SF2 stereo pairs are two independent mono samples stored in a stereo
  // frame with the other half silent: keep only the real channel and let
  // the pan generator position it (FluidSynth model, halves the memory)
  const bool collapse = channels == 2 && takeChannel >= 0 && takeChannel <= 1;
  const int outChannels = collapse ? 1 : std::max(1, channels);

  // The file libraries do not guarantee that LoadSampleData() caches as many
  // frames as the headers advertise (truncated / corrupt files): never trust
  // the header over the byte count actually returned.
  const int64_t frameSize = int64_t(std::max(1, channels)) * (bitDepth / 8);
  totalSamples = std::min(totalSamples, rawSize / frameSize);
  if(totalSamples <= 0)
    return result;

  ossia::audio_array out_data;
  out_data.resize(outChannels);
  for(auto& ch : out_data)
    ch.resize(totalSamples);

  const auto* raw = static_cast<const uint8_t*>(rawData);

  if(bitDepth == 16)
  {
    const auto* samples = reinterpret_cast<const int16_t*>(raw);
    constexpr double scale = 1.0 / 32768.0;
    if(channels == 1)
    {
      for(int64_t i = 0; i < totalSamples; i++)
        out_data[0][i] = samples[i] * scale;
    }
    else if(collapse)
    {
      for(int64_t i = 0; i < totalSamples; i++)
        out_data[0][i] = samples[i * 2 + takeChannel] * scale;
    }
    else if(channels == 2)
    {
      for(int64_t i = 0; i < totalSamples; i++)
      {
        out_data[0][i] = samples[i * 2] * scale;
        out_data[1][i] = samples[i * 2 + 1] * scale;
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
        out_data[0][i] = s * scale;
      }
    }
    else if(collapse)
    {
      for(int64_t i = 0; i < totalSamples; i++)
      {
        const int64_t off = i * 6 + takeChannel * 3;
        int32_t v = (int32_t)raw[off] | ((int32_t)raw[off + 1] << 8)
                    | ((int32_t)(int8_t)raw[off + 2] << 16);
        out_data[0][i] = v * scale;
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
        out_data[0][i] = l * scale;
        out_data[1][i] = r * scale;
      }
    }
  }
  else if(bitDepth == 8)
  {
    constexpr double scale = 1.0 / 128.0;
    const int offset = signed8 ? 0 : 128;
    const auto s8 = [&](uint8_t v) {
      return signed8 ? (double)(int8_t)v * scale : ((int)v - offset) * scale;
    };
    if(channels == 1)
    {
      for(int64_t i = 0; i < totalSamples; i++)
        out_data[0][i] = s8(raw[i]);
    }
    else if(collapse)
    {
      for(int64_t i = 0; i < totalSamples; i++)
        out_data[0][i] = s8(raw[i * 2 + takeChannel]);
    }
    else if(channels == 2)
    {
      for(int64_t i = 0; i < totalSamples; i++)
      {
        out_data[0][i] = s8(raw[i * 2]);
        out_data[1][i] = s8(raw[i * 2 + 1]);
      }
    }
  }

  return resampleArray(std::move(out_data), sourceRate, targetRate);
}

/////////////////////////////
// External audio files (drumkit layers, KORG-less formats)
/////////////////////////////

struct DecodedAudioFile
{
  std::shared_ptr<ossia::audio_array> data;
  uint32_t rate{};
};

struct AudioCacheEntry
{
  std::string path;
  FileStamp stamp;
  int targetRate{};
  uint32_t rate{};
  std::size_t bytes{};
  std::shared_ptr<ossia::audio_array> data;
};

struct AudioCache
{
  std::mutex mutex;
  std::deque<AudioCacheEntry> entries; // most recent first
  std::size_t bytes{};
};
AudioCache& audioCache()
{
  static AudioCache c;
  return c;
}

// A drumkit is a few tens of megabytes; this holds a handful of them so that
// re-picking a kit, or two processes playing the same one, decode nothing.
constexpr std::size_t kMaxCachedAudioBytes = 256ull * 1024 * 1024;

std::optional<DecodedAudioFile>
cachedAudio(const std::string& path, const FileStamp& stamp, int targetRate)
{
  if(!stamp.valid())
    return std::nullopt;

  auto& c = audioCache();
  std::lock_guard lock{c.mutex};
  for(auto it = c.entries.begin(); it != c.entries.end(); ++it)
  {
    if(it->path != path || it->targetRate != targetRate)
      continue;
    if(it->stamp == stamp)
    {
      auto hit = std::move(*it);
      c.entries.erase(it);
      DecodedAudioFile res{hit.data, hit.rate};
      c.entries.push_front(std::move(hit));
      counters().audioHits.fetch_add(1, std::memory_order_relaxed);
      return res;
    }
    c.bytes -= it->bytes;
    c.entries.erase(it); // the file changed underneath us
    break;
  }
  return std::nullopt;
}

void cacheAudio(
    std::string path, const FileStamp& stamp, int targetRate,
    const DecodedAudioFile& decoded)
{
  if(!stamp.valid() || !decoded.data)
    return;

  std::size_t bytes = 0;
  for(const auto& ch : *decoded.data)
    bytes += ch.size() * sizeof(ossia::audio_sample);
  if(bytes > kMaxCachedAudioBytes)
    return;

  auto& c = audioCache();
  std::lock_guard lock{c.mutex};
  // Two loads of the same file can race to decode it; keep one entry
  std::erase_if(c.entries, [&](const AudioCacheEntry& e) {
    if(e.path != path || e.targetRate != targetRate)
      return false;
    c.bytes -= e.bytes;
    return true;
  });

  c.entries.push_front({std::move(path), stamp, targetRate, decoded.rate, bytes,
                        decoded.data});
  c.bytes += bytes;
  while(c.bytes > kMaxCachedAudioBytes && c.entries.size() > 1)
  {
    c.bytes -= c.entries.back().bytes;
    c.entries.pop_back();
  }
}

// Plain PCM wav/w64 already at the engine rate are read with dr_wav over a
// memory mapping, which skips libav's per-file demuxer, decoder and
// per-channel resampler setup. The mapping is faulted in and dropped here,
// on the loader thread: what the engine gets is an ossia::audio_array like
// every other path.
//
// A rate mismatch sends the file back to libav, the same gate
// Media::needsDecoding applies: swr converts the rate for free inside the
// decode, where the windowed-sinc resampler below would cost several times
// what the decode saves.
std::optional<DecodedAudioFile> decodeWithDrwav(
    const QString& path, int targetRate,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  QFile f{path};
  if(!f.open(QIODevice::ReadOnly))
    return std::nullopt;
  const auto size = f.size();
  if(size <= 0)
    return std::nullopt;
  uchar* const map = f.map(0, size);
  if(!map)
    return std::nullopt;

  struct Unmapper
  {
    QFile& file;
    uchar* addr;
    ~Unmapper() { file.unmap(addr); }
  } unmapper{f, map};

  ossia::drwav_handle h;
  h.open_memory(map, (std::size_t)size);
  // open_memory() does not report a failed init: dr_wav leaves a zeroed
  // handle for the wav containers it cannot read
  if(!h)
    return std::nullopt;
  const auto channels = h.channels();
  const auto rate = h.sampleRate();
  const auto frames = (int64_t)h.totalPCMFrameCount();
  if(channels == 0 || rate == 0 || frames <= 0 || rate != (uint32_t)targetRate)
    return std::nullopt;
  switch(h.translatedFormatTag())
  {
    case DR_WAVE_FORMAT_PCM:
    case DR_WAVE_FORMAT_IEEE_FLOAT:
    case DR_WAVE_FORMAT_ALAW:
    case DR_WAVE_FORMAT_MULAW:
      break;
    default:
      // DTS, GSM and the ADPCM flavours dr_wav declines are all legal in a
      // wav container: leave those to libav
      return std::nullopt;
  }

  ossia::audio_array out;
  out.resize(channels);
  for(auto& ch : out)
    ch.resize(frames);

  // Chunked, so that a long file still polls the cancel token; the frame
  // count follows the channel count so that the interleaved staging buffer
  // stays cache-sized whatever the header claims
  const int64_t chunkFrames = std::clamp<int64_t>(65536 / channels, 1, 8192);
  std::vector<float> interleaved(chunkFrames * channels);
  int64_t pos = 0;
  while(pos < frames)
  {
    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return std::nullopt;

    const int64_t want = std::min(chunkFrames, frames - pos);
    const int64_t got = (int64_t)h.read_pcm_frames_f32(want, interleaved.data());
    if(got <= 0)
      break;

    for(int64_t i = 0; i < got; i++)
    {
      for(uint32_t c = 0; c < channels; c++)
      {
        // Float wavs can legitimately carry NaN / inf, and a NaN sticks in
        // the engine's filters forever
        const float v = interleaved[i * channels + c];
        out[c][pos + i] = std::isfinite(v) ? v : 0.f;
      }
    }
    pos += got;
  }
  if(pos < frames) // truncated file
    for(auto& ch : out)
      ch.resize(pos);
  if(pos <= 0)
    return std::nullopt;

  return DecodedAudioFile{
      std::make_shared<ossia::audio_array>(std::move(out)), rate};
}

std::optional<DecodedAudioFile> decodeAudioFile(
    const QString& path, int targetRate,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  const auto suffix = QFileInfo(path).suffix().toLower();
  if(suffix == "wav" || suffix == "w64")
  {
    if(auto decoded = decodeWithDrwav(path, targetRate, cancelled))
    {
      counters().drwavDecodes.fetch_add(1, std::memory_order_relaxed);
      return decoded;
    }
    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return std::nullopt;
  }

  auto dec = Media::AudioDecoder::decode_synchronous(path, targetRate);
  if(!dec)
    return std::nullopt;
  counters().libavDecodes.fetch_add(1, std::memory_order_relaxed);

  auto arr = std::make_shared<ossia::audio_array>(std::move(dec->second));
  for(auto& channel : *arr)
    for(auto& sample : channel)
      if(!std::isfinite(sample))
        sample = 0.;
  return DecodedAudioFile{std::move(arr), (uint32_t)targetRate};
}

std::optional<DecodedAudioFile> loadAudioFile(
    const QString& path, int targetRate,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  auto key = path.toStdString();
  const auto stamp = stampOf(path);
  if(auto hit = cachedAudio(key, stamp, targetRate))
    return hit;

  counters().audioDecodes.fetch_add(1, std::memory_order_relaxed);
  auto decoded = decodeAudioFile(path, targetRate, cancelled);
  if(!decoded)
    return std::nullopt;
  cacheAudio(std::move(key), stamp, targetRate, *decoded);
  return decoded;
}

// Convert each unique source sample once, share the decoded audio between
// its regions, rescale per-region loop points and offsets, then drop regions
// that ended up without sample data.
void convertAndPrune(
    std::vector<RawSampleBuffer>& rawBuffers, GigInstrument& destInstr,
    int targetRate, const std::shared_ptr<std::atomic<bool>>& cancelled,
    bool& wasCancelled)
{
  wasCancelled = false;

  // Converting a raw buffer reads only that buffer and writes only its own
  // ConvertedSample, so the buffers fan out cleanly. The workers are plain
  // threads rather than TaskPool tasks: this function already runs on a
  // TaskPool worker, and posting back into the same pool and waiting is the
  // deadlock the sequential version was avoiding.
  std::vector<ConvertedSample> converted(rawBuffers.size());
  {
    std::size_t totalBytes = 0;
    for(const auto& raw : rawBuffers)
      totalBytes += raw.data.size();

    std::atomic<std::size_t> next{0};
    std::atomic<bool> aborted{false};
    const auto work = [&] {
      for(std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
          i < rawBuffers.size(); i = next.fetch_add(1, std::memory_order_relaxed))
      {
        if(aborted.load(std::memory_order_relaxed)
           || (cancelled && cancelled->load(std::memory_order_relaxed)))
        {
          aborted.store(true, std::memory_order_relaxed);
          return;
        }

        auto& raw = rawBuffers[i];
        try
        {
          converted[i] = convertRawSampleData(
              raw.data.data(), raw.data.size(), raw.channels, raw.bitDepth,
              raw.totalSamples, raw.sourceRate, targetRate, raw.signed8,
              raw.takeChannel);
        }
        catch(...)
        {
          // Out of memory on a hostile header: give up on the whole load
          // rather than let the exception cross a thread boundary
          aborted.store(true, std::memory_order_relaxed);
          return;
        }
        raw.data = {}; // the source bytes are dead once converted
      }
    };

    // Small banks are not worth the thread handshake; the threshold is in
    // bytes because it is the conversion, not the buffer count, that costs.
    constexpr std::size_t kParallelThreshold = 1024 * 1024;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t workers
        = (rawBuffers.size() < 2 || totalBytes < kParallelThreshold)
              ? 1
              : std::min<std::size_t>({rawBuffers.size(), hw, 8});

    if(workers > 1)
      counters().parallelConversions.fetch_add(1, std::memory_order_relaxed);

    std::vector<std::thread> helpers;
    helpers.reserve(workers - 1);
    try
    {
      for(std::size_t w = 1; w < workers; w++)
        helpers.emplace_back(work);
    }
    catch(...)
    {
      // Out of threads: the ones that did start share the same queue, and
      // this thread takes the rest. Letting the exception out here would
      // unwind past helpers still reading these locals.
    }
    work();
    for(auto& t : helpers)
      t.join();

    if(aborted.load(std::memory_order_relaxed))
    {
      wasCancelled = true;
      return;
    }
  }

  for(std::size_t i = 0; i < rawBuffers.size(); i++)
  {
    const auto& raw = rawBuffers[i];
    const auto& conv = converted[i];
    if(!conv.data || conv.data->empty() || (*conv.data)[0].empty())
      continue;
    const int64_t frames = (int64_t)(*conv.data)[0].size();

    for(const int regionIndex : raw.regionIndices)
    {
      auto& region = destInstr.regions[regionIndex];
      auto& smp = region.sample;
      smp.data = conv.data;
      smp.sampleRate = conv.rate;

      if(conv.ratio != 1.0)
      {
        if(smp.hasLoop)
        {
          smp.loopStart = (uint32_t)(smp.loopStart * conv.ratio);
          smp.loopEnd = (uint32_t)(smp.loopEnd * conv.ratio);
        }
        if(region.sampleStartOffset > 0)
          region.sampleStartOffset = (uint32_t)std::clamp<int64_t>(
              std::llround(region.sampleStartOffset * conv.ratio), 0, INT32_MAX);
        if(region.sampleEndOffset > 0)
          region.sampleEndOffset = (uint32_t)std::clamp<int64_t>(
              std::llround(region.sampleEndOffset * conv.ratio), 0, INT32_MAX);
      }

      // Clamp loop points to the decoded frame count: files can declare
      // LoopStart/LoopEnd past the actual sample data, and the render loop
      // indexes the buffer with these values.
      if(smp.hasLoop)
      {
        if((int64_t)smp.loopEnd > frames)
          smp.loopEnd = (uint32_t)frames;
        if(smp.loopStart >= smp.loopEnd)
          smp.hasLoop = false;
      }
    }
  }

  // Remove regions that still have no sample data (failed to load)
  std::erase_if(destInstr.regions, [](const GigRegion& r) {
    return !r.sample.data || r.sample.data->empty() || (*r.sample.data)[0].empty();
  });

  // Region indices changed: recompute the alternation groups
  assignAlternationGroups(destInstr);
}

/////////////////////////////
// GigaStudio / Gigasampler
/////////////////////////////

// True when this dimension region sits in a non-default zone of a selector
// dimension we do not model (keyswitch, mod wheel, sustain pedal,
// aftertouch, smart MIDI, ...): only the zone the selector rests in (zone 0)
// plays, otherwise every keyswitched articulation would stack on top of each
// other. Layers and sample channels stack by design; velocity, round-robin,
// random and release-trigger zones are modelled properly.
// Used by both the metadata and the raw-buffer walk, which must stay in
// lockstep.
bool gigZoneIsUnselected(const gig::Region* rgn, uint32_t d)
{
  int bitsBelow = 0;
  for(unsigned int dim = 0; dim < rgn->Dimensions;
      bitsBelow += rgn->pDimensionDefinitions[dim].bits, dim++)
  {
    switch(rgn->pDimensionDefinitions[dim].dimension)
    {
      case gig::dimension_velocity:
      case gig::dimension_roundrobin:
      case gig::dimension_roundrobinkeyboard:
      case gig::dimension_random:
      case gig::dimension_releasetrigger:
      case gig::dimension_layer:
      case gig::dimension_samplechannel:
        break;
      default:
      {
        const int mask = (1 << rgn->pDimensionDefinitions[dim].bits) - 1;
        if(((d >> bitsBelow) & mask) != 0)
          return true;
        break;
      }
    }
  }
  return false;
}

std::shared_ptr<GigFileInfo>
loadMetadata_gig(const QString& filePath, gig::File* gigFile, int instrumentIndex)
{
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

      if(gigZoneIsUnselected(rgn, d))
        continue;

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

      // Filter. When the cutoff is bound to a MIDI controller, the stored
      // byte is only the controller's fallback value (often 0): without
      // live controller input, play fully open instead of nearly silent.
      region.vcfEnabled = dimRgn->VCFEnabled;
      const auto cutCtl = dimRgn->VCFCutoffController;
      if(cutCtl != gig::vcf_cutoff_ctrl_none && cutCtl != gig::vcf_cutoff_ctrl_none2)
        region.vcfCutoff = 127;
      else
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
      region.sample.midiUnityNote = std::min<uint32_t>(smp->MIDIUnityNote, 127);
      region.sample.fineTune = smp->FineTune;
      if(smp->Loops > 0)
      {
        region.sample.hasLoop = true;
        region.sample.loopStart = smp->LoopStart;
        // libgig's LoopEnd is the inclusive last sample of the loop; the
        // engine wraps at loop.end exclusively
        region.sample.loopEnd = smp->LoopEnd + 1;
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
    const GigFileInfo& info, gig::File* gigFile,
    std::vector<RawSampleBuffer>& rawBuffers,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  gig::Instrument* gigInstr = gigFile->GetInstrument(info.selectedInstrument);
  if(!gigInstr)
    return false;

  const auto& destInstr = info.instruments[info.selectedInstrument];

  // Regions frequently share their sample (velocity layers, stereo pairs):
  // read and store each source only once
  std::unordered_map<const void*, std::size_t> seen;

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

      if(gigZoneIsUnselected(rgn, d))
        continue;

      if(regionIdx >= (int)destInstr.regions.size())
        break;

      if(auto it = seen.find(smp); it != seen.end())
      {
        rawBuffers[it->second].regionIndices.push_back(regionIdx);
      }
      else
      {
        // Load raw sample data from the gig file
        gig::buffer_t buf = smp->LoadSampleData();
        if(buf.pStart && buf.Size > 0)
        {
          RawSampleBuffer raw;
          const auto* bytes = static_cast<const uint8_t*>(buf.pStart);
          raw.data.assign(bytes, bytes + buf.Size);
          raw.channels = smp->Channels;
          raw.bitDepth = smp->BitDepth;
          raw.totalSamples = smp->SamplesTotal;
          raw.sourceRate = smp->SamplesPerSecond;
          raw.regionIndices.push_back(regionIdx);
          seen[smp] = rawBuffers.size();
          rawBuffers.push_back(std::move(raw));
        }
        smp->ReleaseSampleData();
      }

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

uint8_t frequencyToVcfCutoff(double freq);

// Gain is stored as 32 bit fixed point relative gain in dB (1/655360 dB
// units). Per the DLS spec the value is <= 0 and negative attenuates; some
// gig-style writers store a positive attenuation instead, so never boost:
// treat any sign as attenuation.
double dlsGainToLinear(int32_t gain)
{
  return std::pow(10.0, -std::abs((double)gain) / (20.0 * 655360.0));
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
        case DLS::conn_dst_eg1_delaytime:
          region.eg1Delay = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_attacktime:
          region.eg1Attack = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_holdtime:
          region.eg1Hold = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_decaytime:
          region.eg1Decay = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_releasetime:
          region.eg1Release = dlsTimeCentsToSeconds(scale);
          break;
        case DLS::conn_dst_eg1_sustainlevel:
          // 0.1% units in 16.16 fixed point: 0 .. 1000 = 0 .. 100%
          region.eg1Sustain = std::clamp(scale / 65536.0 / 1000.0, 0.0, 1.0);
          break;
        case DLS::conn_dst_pan:
          // 0.1% units in 16.16 fixed point: -500 .. +500 = hard left..right
          region.pan = (int8_t)std::clamp<int>(
              (int)std::lround(scale / 655360.0 / 50.0 * 64.0), -64, 63);
          break;
        case DLS::conn_dst_gain:
          // Static per-region level (0.1 dB units in 16.16), multiplicative
          // with the wsmp gain; SF2->DLS converters put layer balance here
          region.sampleAttenuation
              *= std::pow(10.0, -std::abs(scale / 655360.0) / 20.0);
          break;
        case DLS::conn_dst_pitch:
          // Static tuning offset, pitch cents in 16.16
          region.pitchOffset += scale / 65536.0 / 100.0;
          break;
        case DLS::conn_dst_filter_cutoff:
          // Absolute pitch cents (16.16) relative to 8.176 Hz
          if(const double cents = scale / 65536.0; cents > 0 && cents < 13500)
          {
            region.vcfEnabled = true;
            region.vcfCutoff
                = frequencyToVcfCutoff(8.176 * std::pow(2.0, cents / 1200.0));
          }
          break;
        case DLS::conn_dst_filter_q:
          // 0.1 dB units in 16.16 -> centibels
          if(const double qCb = scale / 65536.0; qCb > 0)
          {
            region.vcfEnabled = true;
            region.vcfQCb = (float)std::clamp(qCb, 0.0, 960.0);
          }
          break;
        default:
          break;
      }
    }
  }
}

std::shared_ptr<GigFileInfo>
loadMetadata_dls(const QString& filePath, DLS::File* dlsFile, int instrumentIndex)
{
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

    // wsmp override rule: the region's wsmp chunk overrides the wave's as a
    // whole; when the region has none, the wave's unity note / fine tune /
    // gain / loop table apply (many DLS files only carry wave-level wsmp)
    const bool rgnWsmp = rgn->WavesampleChunkPresent;
    region.sampleAttenuation = dlsGainToLinear(
        rgnWsmp ? rgn->Gain : (smp->WsmpPresent ? smp->WsmpGain : 0));
    region.sample.sampleRate = smp->SamplesPerSecond;
    region.sample.midiUnityNote = std::min<uint32_t>(
        rgnWsmp ? rgn->UnityNote : (smp->WsmpPresent ? smp->WsmpUnityNote : 60), 127);
    region.sample.fineTune
        = rgnWsmp ? rgn->FineTune : (smp->WsmpPresent ? smp->WsmpFineTune : 0);
    // Key groups are defined for drum instruments, valid range 1-15, and a
    // self-non-exclusive region must not choke its own retriggers
    if(dlsInstr->IsDrum && rgn->KeyGroup >= 1 && rgn->KeyGroup <= 15
       && !rgn->SelfNonExclusive)
      region.chokeGroup = rgn->KeyGroup;

    const uint32_t nLoops = rgnWsmp ? rgn->SampleLoops : smp->WsmpSampleLoops;
    const auto* loops = rgnWsmp ? rgn->pSampleLoops : smp->pWsmpSampleLoops;
    if(nLoops > 0 && loops)
    {
      const auto& loop = loops[0];
      if(loop.LoopLength > 0)
      {
        region.sample.hasLoop = true;
        region.sample.loopStart = loop.LoopStart;
        region.sample.loopEnd = loop.LoopStart + loop.LoopLength;
        // DLS loop type 1 is a release loop: loop while the note is held,
        // then play through the tail of the wave
        region.sample.loopType = 0;
        region.sample.loopUntilRelease = loop.LoopType == 1;
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
    const GigFileInfo& info, DLS::File* dlsFile,
    std::vector<RawSampleBuffer>& rawBuffers,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  DLS::Instrument* dlsInstr = dlsFile->GetFirstInstrument();
  for(int i = 0; i < info.selectedInstrument && dlsInstr; i++)
    dlsInstr = dlsFile->GetNextInstrument();
  if(!dlsInstr)
    return false;

  const auto& destInstr = info.instruments[info.selectedInstrument];

  std::unordered_map<const void*, std::size_t> seen;

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

    if(auto it = seen.find(smp); it != seen.end())
    {
      rawBuffers[it->second].regionIndices.push_back(regionIdx);
    }
    else if(void* p = smp->LoadSampleData())
    {
      const int64_t bytes = (int64_t)smp->GetSize() * smp->FrameSize;
      if(bytes > 0)
      {
        RawSampleBuffer raw;
        const auto* src = static_cast<const uint8_t*>(p);
        raw.data.assign(src, src + bytes);
        raw.channels = smp->Channels;
        raw.bitDepth = smp->BitDepth;
        raw.totalSamples = smp->SamplesTotal;
        raw.sourceRate = smp->SamplesPerSecond;
        raw.regionIndices.push_back(regionIdx);
        seen[smp] = rawBuffers.size();
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
loadMetadata_sf2(const QString& filePath, sf2::File* sfFile, int instrumentIndex)
{
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
      // initialAttenuation is in centibels of attenuation (0..1440), scaled
      // by the 0.4 factor of the EMU8k/10k hardware: FluidSynth applies it
      // unconditionally and real-world banks are balanced against it
      region.sampleAttenuation = std::pow(
          10.0, -0.4 * std::clamp(iz->GetInitialAttenuation(pz), 0, 1440) / 200.0);
      // Coarse tune is a separate pitch offset, not a root key change:
      // folding it into the root both clamps wrongly at the MIDI range edges
      // and loses it entirely for unpitched (fixed-pitch) samples
      region.sample.midiUnityNote = (uint32_t)std::clamp(iz->GetUnityNote(), 0, 127);
      region.pitchOffset += iz->GetCoarseTune(pz);
      // scaleTuning: cents of pitch per key step; 0 = fixed pitch
      region.keyScale = iz->GetScaleTuning(pz) / 100.0;
      // The sample header's pitch correction (signed cents) applies on top
      // of the fine tune generators; banks sampled from hardware rely on it
      // heavily and skipping it leaves regions audibly out of tune.
      region.sample.fineTune = iz->GetFineTune(pz) + smp->PitchCorrection;
      region.pan = (int8_t)std::clamp(iz->GetPan(pz), -64, 63);
      region.sampleStartOffset = (uint32_t)std::clamp<int64_t>(
          (int64_t)iz->startAddrsOffset + 32768ll * iz->startAddrsCoarseOffset, 0,
          INT32_MAX);

      // EG1, in the EMU dB-slope semantics the engine implements natively:
      // the decay time is for a full-scale fall and ends at the sustain level
      const double sustainCb = std::clamp(iz->GetEG1Sustain(pz), 0, 1440);
      region.eg1DbSlope = true;
      region.eg1Delay = iz->GetEG1PreAttackDelay(pz);
      region.eg1Attack = iz->GetEG1Attack(pz);
      region.eg1Hold = iz->GetEG1Hold(pz);
      region.eg1Decay = iz->GetEG1Decay(pz);
      region.eg1Sustain = std::pow(10.0, -sustainCb / 200.0);
      region.eg1Release = iz->GetEG1Release(pz);
      // High notes decay faster (timecents per key relative to key 60)
      region.keynumToDecay = iz->GetKeynumToVolEnvDecay(pz);
      region.keynumToHold = iz->GetKeynumToVolEnvHold(pz);

      // Filter: 13500 absolute cents is the "fully open" default, but a
      // non-zero Q still applies its gain change there (FluidSynth never
      // bypasses the filter)
      const int fc = iz->GetInitialFilterFc(pz);
      const int qCb = std::clamp(iz->GetInitialFilterQ(pz), 0, 960);
      if(fc < 13500 || qCb > 0)
      {
        region.vcfEnabled = true;
        region.vcfCutoffHz = 8.176 * std::pow(2.0, fc / 1200.0);
        region.vcfCutoff = frequencyToVcfCutoff(region.vcfCutoffHz);
        region.vcfQCb = qCb;
      }

      // File-specified vibrato
      if(const int vibCents = iz->GetVibLfoToPitch(pz))
      {
        region.vibLfoToPitch = vibCents;
        region.vibLfoFreq = (float)iz->GetFreqVibLfo(pz);
        region.vibLfoDelay = (float)iz->GetDelayVibLfo(pz);
      }

      // File-specified modLfo (tremolo / filter wobble / pitch)
      {
        const int toPitch = iz->GetModLfoToPitch(pz);
        const int toFc = iz->GetModLfoToFilterFc(pz);
        const double toVol = iz->GetModLfoToVolume(pz);
        if(toPitch != 0 || toFc != 0 || toVol != 0.)
        {
          region.modLfoToPitch = toPitch;
          region.modLfoToFc = toFc;
          region.modLfoToVol = (float)toVol;
          region.modLfoFreq = (float)iz->GetFreqModLfo(pz);
          region.modLfoDelay = (float)iz->GetDelayModLfo(pz);
        }
      }

      // Modulation envelope -> pitch / cutoff
      {
        const int toPitch = iz->GetModEnvToPitch(pz);
        const int toFc = iz->GetModEnvToFilterFc(pz);
        if(toPitch != 0 || toFc != 0)
        {
          region.modEnvToPitch = toPitch;
          region.modEnvToFc = toFc;
          region.eg2Delay = (float)iz->GetEG2PreAttackDelay(pz);
          region.eg2Attack = (float)iz->GetEG2Attack(pz);
          region.eg2Hold = (float)iz->GetEG2Hold(pz);
          region.eg2Decay = (float)iz->GetEG2Decay(pz);
          region.eg2Sustain
              = std::clamp(iz->GetEG2Sustain(pz), 0, 1000) / 1000.f;
          region.eg2Release = (float)iz->GetEG2Release(pz);
        }
      }

      // Instrument-zone key / velocity overrides
      region.forcedKey = iz->keynum;
      region.forcedVelocity = iz->velocity;

      // Default velocity->cutoff modulator (SF2 2.01 default #2, -2400
      // cents); a bank modulator with the same source and destination
      // supersedes it, including with amount 0 to disable it
      region.velToFcCents = -2400.f;
      for(const auto& m : iz->modulators)
      {
        if(m.ModDestOper == sf2::INITIAL_FILTER_FC && !m.ModSrcOper.MidiPalete
           && m.ModSrcOper.Index == sf2::Modulator::NOTE_ON_VELOCITY)
        {
          region.velToFcCents = (float)(int16_t)m.ModAmount;
          break;
        }
      }

      // Trailing frames trimmed off the sample end (offsets are <= 0)
      {
        const int64_t endOff = (int64_t)iz->endAddrsOffset
                               + 32768ll * (int64_t)iz->endAddrsCoarseOffset;
        if(endOff < 0)
          region.sampleEndOffset = (uint32_t)std::min<int64_t>(-endOff, INT32_MAX);
      }

      region.sample.sampleRate = smp->SampleRate;
      if(iz->HasLoop)
      {
        region.sample.hasLoop = true;
        region.sample.loopStart = iz->LoopStart;
        region.sample.loopEnd = iz->LoopEnd;
        region.sample.loopUntilRelease = iz->LoopUntilRelease;
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
    const GigFileInfo& info, sf2::File* sfFile,
    std::vector<RawSampleBuffer>& rawBuffers,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  sf2::Preset* preset = sfFile->GetPreset(info.selectedInstrument);
  if(!preset)
    return false;

  const auto& destInstr = info.instruments[info.selectedInstrument];

  std::unordered_map<const void*, std::size_t> seen;

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

      if(auto it = seen.find(smp); it != seen.end())
      {
        rawBuffers[it->second].regionIndices.push_back(regionIdx);
      }
      else
      {
        sf2::Sample::buffer_t buf = smp->LoadSampleData();
        if(buf.pStart && buf.Size > 0)
        {
          RawSampleBuffer raw;
          const auto* bytes = static_cast<const uint8_t*>(buf.pStart);
          raw.data.assign(bytes, bytes + buf.Size);
          raw.channels = smp->GetChannelCount();
          raw.bitDepth
              = smp->GetChannelCount() > 0
                    ? 8 * (smp->GetFrameSize() / smp->GetChannelCount())
                    : 16;
          raw.totalSamples = smp->GetTotalFrameCount();
          raw.sourceRate = smp->SampleRate;
          // Stereo pairs are two mono samples: keep only the real channel
          // (libgig decodes left into slot 0, right into slot 1)
          switch(smp->SampleType)
          {
            case sf2::Sample::LEFT_SAMPLE:
            case sf2::Sample::ROM_LEFT_SAMPLE:
              raw.takeChannel = 0;
              break;
            case sf2::Sample::RIGHT_SAMPLE:
            case sf2::Sample::ROM_RIGHT_SAMPLE:
              raw.takeChannel = 1;
              break;
            default:
              break;
          }
          raw.regionIndices.push_back(regionIdx);
          seen[smp] = rawBuffers.size();
          rawBuffers.push_back(std::move(raw));
        }
        smp->ReleaseSampleData();
      }

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

// Minimal DOM built with QXmlStreamReader so the loader does not depend on
// the QtXml module (absent from some Qt deployments); mirrors the small
// QDomElement surface the drumkit parser needs.
struct XmlElement
{
  QString tag;
  QString textContent;
  std::vector<XmlElement> children;

  bool isNull() const noexcept { return tag.isEmpty(); }
  const QString& text() const noexcept { return textContent; }

  const XmlElement& firstChildElement(const char* name) const noexcept
  {
    static const XmlElement null_element;
    for(const auto& c : children)
      if(c.tag == QLatin1StringView(name))
        return c;
    return null_element;
  }

  // All direct children with the given tag, in document order
  std::vector<const XmlElement*> childrenNamed(const char* name) const
  {
    std::vector<const XmlElement*> out;
    for(const auto& c : children)
      if(c.tag == QLatin1StringView(name))
        out.push_back(&c);
    return out;
  }
};

bool parseXml(QIODevice& dev, XmlElement& root)
{
  QXmlStreamReader xr(&dev);
  // Ancestors' child vectors only grow while they are top-of-stack, so the
  // raw pointers stay valid for as long as they are on the stack
  std::vector<XmlElement*> stack;
  while(!xr.atEnd())
  {
    switch(xr.readNext())
    {
      case QXmlStreamReader::StartElement:
        if(stack.empty())
        {
          root.tag = xr.name().toString();
          stack.push_back(&root);
        }
        else
        {
          stack.back()->children.push_back(XmlElement{xr.name().toString(), {}, {}});
          stack.push_back(&stack.back()->children.back());
        }
        break;
      case QXmlStreamReader::Characters:
        if(!stack.empty() && !xr.isWhitespace())
          stack.back()->textContent += xr.text();
        break;
      case QXmlStreamReader::EndElement:
        if(!stack.empty())
          stack.pop_back();
        break;
      default:
        break;
    }
  }
  return !xr.hasError() && !root.isNull();
}

// Numeric XML element with a default: missing elements and non-finite or
// unparseable values (hand-edited kits do contain "nan"s) yield the default.
double xmlNumber(const XmlElement& parent, const char* name, double def)
{
  const auto& e = parent.firstChildElement(name);
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

  XmlElement root;
  if(!parseXml(file, root))
    return {};
  file.close();

  if(root.tag != "drumkit_info")
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
  for(const XmlElement* instp :
      root.firstChildElement("instrumentList").childrenNamed("instrument"))
  {
    const XmlElement& inst = *instp;
    const auto name = inst.firstChildElement("name").text();

    int midi_note = base_midi_note;
    if(const auto& midiOutNote = inst.firstChildElement("midiOutNote");
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
    base.noteLabel = name.toStdString();

    // isStopNote (a "stop note" makes note-off cut the sound) is the inverse
    // of one-shot playback
    base.oneShot = inst.firstChildElement("isStopNote").text() != "true";
    base.muted = inst.firstChildElement("isMuted").text() == "true";
    if(const auto& applyVel = inst.firstChildElement("applyVelocity");
       !applyVel.isNull())
      base.applyVelocity = applyVel.text() == "true";

    // Hydrogen >= 1.1: how the layer within a velocity range is picked.
    // In format v2 the element moved inside <instrumentComponent>.
    const auto algoOf = [](const XmlElement& e) {
      const auto& t = e.text();
      return t == QStringLiteral("ROUND_ROBIN") ? 1
             : t == QStringLiteral("RANDOM")    ? 2
                                                : 0;
    };
    if(const auto& algo = inst.firstChildElement("sampleSelectionAlgo");
       !algo.isNull())
      base.selectionAlgo = algoOf(algo);
    else if(const auto& c = inst.firstChildElement("instrumentComponent"); !c.isNull())
      if(const auto& algo2 = c.firstChildElement("sampleSelectionAlgo");
         !algo2.isNull())
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
    auto addLayer = [&](const XmlElement& layerElem) {
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

    const auto layers = inst.childrenNamed("layer");
    const auto& component = inst.firstChildElement("instrumentComponent");
    if(layers.empty() && component.isNull())
    {
      // Oldest schema: a single <filename> directly on the instrument
      addLayer(inst);
    }
    else if(!layers.empty())
    {
      for(const XmlElement* l : layers)
        addLayer(*l);
    }
    else
    {
      for(const XmlElement* l : component.childrenNamed("layer"))
        addLayer(*l);
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
      // Layers sharing a file, and kits loaded again later, both come back
      // out of the decoded-file cache
      if(auto dec = loadAudioFile(
             QString::fromStdString(region.sample.sourceFile), targetRate,
             cancelled))
      {
        region.sample.data = std::move(dec->data);
        region.sample.sampleRate = dec->rate;
      }
    }
    catch(...)
    {
      // Undecodable layers are pruned with the other empty regions
    }
  }
  return true;
}

/////////////////////////////
// KORG .KMP multisamples
/////////////////////////////

// libgig assumes .KSF files live in a directory named after the .KMP. Real
// banks keep them as siblings, in per-floppy subdirectories ("disk 2"), or
// in shared folders of the bank collection, and DOS-era media mixes case
// freely. This index maps lowercased file names to paths, first across the
// KMP's own directory tree, then (on miss) across the parent collection.
struct KorgSampleIndex
{
  QDir kmpDir;
  QHash<QString, QString> files;
  int level{0}; // 0: not built, 1: kmp dir tree, 2: + parent tree

  explicit KorgSampleIndex(const QString& kmpPath)
      : kmpDir{QFileInfo(kmpPath).dir()}
  {
  }

  void indexTree(const QString& root)
  {
    QDirIterator it{root, QDir::Files, QDirIterator::Subdirectories};
    while(it.hasNext())
    {
      it.next();
      const auto name = it.fileName().toLower();
      if(!files.contains(name))
        files.insert(name, it.filePath());
    }
  }

  std::string resolve(const std::string& libgigGuess, const std::string& bareName)
  {
    if(QFileInfo::exists(QString::fromStdString(libgigGuess)))
      return libgigGuess;

    const QString bare = QString::fromStdString(bareName).toLower();
    if(level < 1)
    {
      level = 1;
      indexTree(kmpDir.absolutePath());
    }
    if(auto it = files.constFind(bare); it != files.constEnd())
      return it->toStdString();

    if(level < 2)
    {
      level = 2;
      QDir parent = kmpDir;
      if(parent.cdUp())
        indexTree(parent.absolutePath());
      if(auto it = files.constFind(bare); it != files.constEnd())
        return it->toStdString();
    }

    return libgigGuess; // let phase 2 fail and prune the region
  }
};

// A .KMP maps contiguous key ranges to mono .KSF sample files; regions are
// chained by TopKey (a region starts right above the previous one's top).
std::shared_ptr<GigFileInfo>
loadMetadata_korg(const QString& filePath, int instrumentIndex)
{
  Korg::KMPInstrument kmp(filePath.toStdString());

  auto info = std::make_shared<GigFileInfo>();
  info->filePath = filePath.toStdString();
  info->selectedInstrument = 0;
  info->name = kmp.Name();
  if(info->name.empty())
    info->name = QFileInfo(filePath).completeBaseName().toStdString();

  GigInstrument instr;
  instr.name = info->name;

  KorgSampleIndex index{filePath};

  int keyLow = 0;
  for(int i = 0; i < kmp.GetRegionCount(); i++)
  {
    Korg::KMPRegion* rgn = kmp.GetRegion(i);
    if(!rgn)
      continue;
    const int keyHigh = std::clamp<int>(rgn->TopKey, 0, 127);

    // "SKIPPEDSAMPL" and "INTERNALnnnn" reference samples that only exist
    // inside the original hardware: nothing to load
    const auto& sampleName = rgn->SampleFileName;
    const bool resolvable = sampleName.rfind("SKIPPEDSAMPL", 0) != 0
                            && sampleName.rfind("INTERNAL", 0) != 0;
    if(resolvable && keyLow <= keyHigh)
    {
      GigRegion region;
      region.keyLow = (uint8_t)std::clamp(keyLow, 0, 127);
      region.keyHigh = (uint8_t)keyHigh;
      region.pitchTrack = rgn->Transpose;
      region.sample.midiUnityNote = std::min<uint32_t>(rgn->OriginalKey, 127);
      region.sample.fineTune = std::clamp<int>(rgn->Tune, -99, 99);
      region.pan = (int8_t)std::clamp<int>(rgn->Pan, -64, 63);
      region.sample.sourceFile
          = index.resolve(rgn->FullSampleFileName(), sampleName);
      sanitizeRegion(region);
      instr.regions.push_back(std::move(region));
    }
    keyLow = keyHigh + 1;
  }

  if(instr.regions.empty())
    return {};
  info->instruments.push_back(std::move(instr));
  return info;
}

// Phase 2 for KORG: read each referenced .KSF once (they are shared between
// KMPs of a bank), also picking up the loop points stored in the sample file
bool collectRawBuffers_korg(
    GigFileInfo& info, std::vector<RawSampleBuffer>& rawBuffers,
    const std::shared_ptr<std::atomic<bool>>& cancelled)
{
  auto& destInstr = info.instruments[info.selectedInstrument];
  std::unordered_map<std::string, std::size_t> seen;

  for(std::size_t regionIdx = 0; regionIdx < destInstr.regions.size(); regionIdx++)
  {
    if(cancelled && cancelled->load(std::memory_order_relaxed))
      return false;

    auto& region = destInstr.regions[regionIdx];
    const auto& src = region.sample.sourceFile;
    if(src.empty())
      continue;

    if(auto it = seen.find(src); it != seen.end())
    {
      auto& raw = rawBuffers[it->second];
      raw.regionIndices.push_back((int)regionIdx);
      region.sample.sampleRate = raw.sourceRate;
      continue;
    }

    try
    {
      Korg::KSFSample ksf(src);

      const auto buf = ksf.LoadSampleData();
      // libgig's IsCompressed() (attribute bit 0x10) misfires on plain-PCM
      // Trinity files: trust the payload size instead — when it matches
      // SamplePoints * frame size the data is raw PCM whatever the flag says.
      const int64_t expected
          = (int64_t)ksf.SamplePoints * std::max(1, ksf.FrameSize());
      const bool rawPcm
          = buf.pStart && expected > 0 && (int64_t)buf.Size >= expected;
      if(!rawPcm && ksf.IsCompressed())
      {
        // Genuinely compressed Korg data is not implemented by libgig
        ksf.ReleaseSampleData();
        continue;
      }
      if(buf.pStart && buf.Size > 0)
      {
        RawSampleBuffer raw;
        const auto* bytes = static_cast<const uint8_t*>(buf.pStart);
        raw.data.assign(bytes, bytes + buf.Size);
        raw.channels = std::max<int>(1, ksf.Channels);
        raw.bitDepth = ksf.BitDepth;
        raw.signed8 = true; // Korg 8-bit samples are signed
        raw.totalSamples = ksf.SamplePoints;
        raw.sourceRate = ksf.SampleRate;
        raw.regionIndices.push_back((int)regionIdx);

        region.sample.sampleRate = ksf.SampleRate;
        if(ksf.LoopEnd > ksf.LoopStart
           && (int64_t)ksf.LoopEnd <= (int64_t)ksf.SamplePoints)
        {
          region.sample.hasLoop = true;
          region.sample.loopStart = ksf.LoopStart;
          region.sample.loopEnd = ksf.LoopEnd;
        }

        seen[src] = rawBuffers.size();
        rawBuffers.push_back(std::move(raw));
      }
      ksf.ReleaseSampleData();
    }
    catch(...)
    {
      // Missing or unreadable .KSF: the region is pruned with the rest
    }
  }
  return true;
}

/////////////////////////////
// Plain audio files
/////////////////////////////

// A bare wav/flac/... maps to one full-range chromatic region rooted at C4;
// phase 2 decodes it like a Hydrogen layer.
std::shared_ptr<GigFileInfo> loadMetadata_audiofile(const QString& filePath)
{
  if(!QFileInfo::exists(filePath))
    return {};

  auto info = std::make_shared<GigFileInfo>();
  info->filePath = filePath.toStdString();
  info->selectedInstrument = 0;
  info->name = QFileInfo(filePath).completeBaseName().toStdString();

  GigInstrument instr;
  instr.name = info->name;

  GigRegion region;
  region.keyLow = 0;
  region.keyHigh = 127;
  region.pitchTrack = true;
  region.sample.midiUnityNote = 60;
  region.sample.sourceFile = filePath.toStdString();
  sanitizeRegion(region);
  instr.regions.push_back(std::move(region));

  info->instruments.push_back(std::move(instr));
  return info;
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
  if(format == SampleFileFormat::Korg)
  {
    try
    {
      if(auto info = loadMetadata_korg(filePath, 0))
        names.push_back(info->name);
    }
    catch(...)
    {
    }
    return names;
  }
  if(format == SampleFileFormat::AudioFile)
  {
    if(QFileInfo::exists(filePath))
      names.push_back(QFileInfo(filePath).completeBaseName().toStdString());
    return names;
  }

  try
  {
    // The library scanner walks a whole sample collection once: it reads a
    // warm bank but must not push thousands of files through the cache.
    auto bank = openBank(filePath, format, false);
    switch(format)
    {
      case SampleFileFormat::Dls: {
        auto* f = bank->dls.get();
        for(auto* in = f->GetFirstInstrument(); in; in = f->GetNextInstrument())
          names.push_back(in->pInfo ? in->pInfo->Name : std::string{});
        break;
      }
      case SampleFileFormat::Sf2: {
        auto* f = bank->sf2.get();
        for(int i = 0, n = f->GetPresetCount(); i < n; i++)
        {
          auto* p = f->GetPreset(i);
          names.push_back(p ? p->Name : std::string{});
        }
        break;
      }
      case SampleFileFormat::Gig: {
        auto* f = bank->gig.get();
        for(auto* in = f->GetFirstInstrument(); in; in = f->GetNextInstrument())
          names.push_back(in->pInfo ? in->pInfo->Name : std::string{});
        break;
      }
      case SampleFileFormat::Hydrogen:
      case SampleFileFormat::Korg:
      case SampleFileFormat::AudioFile:
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
    case SampleFileFormat::Korg:
      return QStringLiteral("KORG");
    case SampleFileFormat::AudioFile:
      return QStringLiteral("Audio");
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
    const auto format = formatForPath(filePath);
    switch(format)
    {
      case SampleFileFormat::Hydrogen:
        return loadMetadata_hydrogen(filePath, instrumentIndex);
      case SampleFileFormat::Korg:
        return loadMetadata_korg(filePath, instrumentIndex);
      case SampleFileFormat::AudioFile:
        return loadMetadata_audiofile(filePath);
      default:
        break;
    }

    auto bank = openBank(filePath, format, true);
    switch(format)
    {
      case SampleFileFormat::Dls:
        return loadMetadata_dls(filePath, bank->dls.get(), instrumentIndex);
      case SampleFileFormat::Sf2:
        return loadMetadata_sf2(filePath, bank->sf2.get(), instrumentIndex);
      case SampleFileFormat::Gig:
        return loadMetadata_gig(filePath, bank->gig.get(), instrumentIndex);
      default:
        break;
    }
    return {};
  }
  catch(RIFF::Exception& e)
  {
    if(e.Message.find("Unsupported version") != std::string::npos)
      qWarning() << "Ogg-compressed SoundFonts (.sf3) are not supported:" << filePath;
    else
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

    const auto path = QString::fromStdString(info->filePath);
    const auto format = formatForPath(path);

    bool ok{};
    switch(format)
    {
      case SampleFileFormat::Hydrogen:
      case SampleFileFormat::AudioFile:
        // Decodes external audio files directly; nothing to convert, but the
        // shared pruning of empty regions below still applies
        ok = loadSamples_hydrogen(*info, targetRate, cancelled);
        break;
      case SampleFileFormat::Korg:
        ok = collectRawBuffers_korg(*info, rawBuffers, cancelled);
        break;
      default: {
        // The bank the metadata phase parsed is still warm: this reads the
        // samples out of that same gig/DLS/sf2 object.
        auto bank = openBank(path, format, true);
        switch(format)
        {
          case SampleFileFormat::Dls:
            ok = collectRawBuffers_dls(*info, bank->dls.get(), rawBuffers, cancelled);
            break;
          case SampleFileFormat::Sf2:
            ok = collectRawBuffers_sf2(*info, bank->sf2.get(), rawBuffers, cancelled);
            break;
          case SampleFileFormat::Gig:
            ok = collectRawBuffers_gig(*info, bank->gig.get(), rawBuffers, cancelled);
            break;
          default:
            break;
        }
        break;
      }
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

LoaderCacheStats loaderCacheStats()
{
  const auto& c = counters();
  return {
      c.bankParses.load(std::memory_order_relaxed),
      c.bankHits.load(std::memory_order_relaxed),
      c.audioDecodes.load(std::memory_order_relaxed),
      c.audioHits.load(std::memory_order_relaxed),
      c.drwavDecodes.load(std::memory_order_relaxed),
      c.libavDecodes.load(std::memory_order_relaxed),
      c.parallelConversions.load(std::memory_order_relaxed)};
}

void clearLoaderCaches()
{
  {
    auto& c = bankCache();
    std::lock_guard lock{c.mutex};
    c.entries.clear();
  }
  {
    auto& c = audioCache();
    std::lock_guard lock{c.mutex};
    c.entries.clear();
    c.bytes = 0;
  }
}

}
