#include "BankNames.hpp"

#include <QFile>

#include <algorithm>
#include <cstring>
#include <exception>

namespace Deuterium::Gig
{
namespace
{
// Thrown as soon as a file departs from what this reader can vouch for; the
// file is then enumerated by libgig.
struct Unsure
{
  const char* why;
};

void require(bool ok, const char* why)
{
  if(!ok)
    throw Unsure{why};
}

constexpr uint32_t cc(const char (&s)[5]) noexcept
{
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8
         | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}

// Sizes read from a file bound every allocation and loop below
constexpr uint64_t maxTextSize = 1 << 20;
constexpr uint64_t maxTableSize = 256 << 20;
constexpr uint32_t maxSubChunks = 1 << 22;
constexpr uint32_t maxSampleLoops = 4096;
constexpr uint32_t maxWavePoolEntries = 1 << 24;
constexpr uint32_t maxScriptSlots = 4096;

// Positioned reads through one read-ahead window. The chunk headers of a
// bank come in runs (an instrument's regions) separated by jumps over sample
// data: the window grows while reads follow each other and restarts small
// after a jump. Reads rather than a mapping, so that a file truncated while
// it is scanned is an error and not a fault.
class FileReader
{
public:
  explicit FileReader(const QString& path)
      : m_file{path}
  {
    require(
        m_file.open(QIODevice::ReadOnly | QIODevice::Unbuffered), "cannot open");
    const auto size = m_file.size();
    require(size >= 0, "no size");
    m_size = uint64_t(size);
  }

  uint64_t size() const noexcept { return m_size; }

  bool contains(uint64_t offset, uint64_t n) const noexcept
  {
    return offset <= m_size && n <= m_size - offset;
  }

  void read(uint64_t offset, void* dst, uint64_t n)
  {
    require(contains(offset, n), "read past the end of the file");
    if(n == 0)
      return;
    if(offset >= m_start && n <= m_buffer.size()
       && offset - m_start <= m_buffer.size() - n)
    {
      std::memcpy(dst, m_buffer.data() + (offset - m_start), n);
      return;
    }
    if(n >= maxWindow)
    {
      fetch(offset, static_cast<char*>(dst), n);
      return;
    }

    const uint64_t end = m_start + m_buffer.size();
    if(m_streaming)
      m_window = maxWindow;
    else if(offset >= end && offset - end < minWindow)
      m_window = std::min(m_window * 4, maxWindow);
    else
      m_window = minWindow;
    const uint64_t length = std::min(std::max(m_window, n), m_size - offset);
    m_buffer.clear();
    m_buffer.resize(length);
    try
    {
      fetch(offset, m_buffer.data(), length);
    }
    catch(...)
    {
      m_buffer.clear();
      throw;
    }
    m_start = offset;
    std::memcpy(dst, m_buffer.data(), n);
  }

  // For a pass over a whole stretch of the file: read ahead at once
  void setStreaming(bool streaming) noexcept { m_streaming = streaming; }

  uint8_t u8(uint64_t offset)
  {
    uint8_t b;
    read(offset, &b, 1);
    return b;
  }
  uint16_t u16(uint64_t offset)
  {
    uint8_t b[2];
    read(offset, b, 2);
    return uint16_t(b[0] | b[1] << 8);
  }
  uint32_t u32(uint64_t offset)
  {
    uint8_t b[4];
    read(offset, b, 4);
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16
           | uint32_t(b[3]) << 24;
  }
  uint64_t u64(uint64_t offset)
  {
    return uint64_t(u32(offset)) | uint64_t(u32(offset + 4)) << 32;
  }

private:
  void fetch(uint64_t offset, char* dst, uint64_t n)
  {
    require(m_file.seek(qint64(offset)), "seek failed");
    uint64_t got = 0;
    while(got < n)
    {
      const auto r = m_file.read(dst + got, qint64(n - got));
      require(r > 0, "short read");
      got += uint64_t(r);
    }
  }

  static constexpr uint64_t minWindow = 4096;
  static constexpr uint64_t maxWindow = 256 * 1024;

  QFile m_file;
  uint64_t m_size{};
  std::vector<char> m_buffer;
  uint64_t m_start{};
  uint64_t m_window{minWindow};
  bool m_streaming{};
};

struct Chunk
{
  uint32_t id{};
  uint32_t listType{}; // lists only
  uint64_t size{};     // of the body; a list's body starts after its type
  uint64_t body{};     // file offset of the body

  bool isList(uint32_t type) const noexcept
  {
    return id == cc("LIST") && listType == type;
  }
};

using Chunks = std::vector<Chunk>;

// RIFF::List::GetSubChunk(): of several chunks with one ID, the last one
const Chunk* lastChunk(const Chunks& chunks, uint32_t id) noexcept
{
  for(auto it = chunks.rbegin(); it != chunks.rend(); ++it)
    if(it->id == id)
      return &*it;
  return nullptr;
}

// RIFF::List::GetSubList(): of several lists with one type, the first one
const Chunk* firstList(const Chunks& chunks, uint32_t type) noexcept
{
  for(const auto& c : chunks)
    if(c.isList(type))
      return &c;
  return nullptr;
}

// A RIFF file as libgig's RIFF::File sees it: same header sizes, same walk
// through a list's sub-chunks, same bounds on what can be read of a chunk.
class Riff
{
public:
  explicit Riff(const QString& path)
      : m_in{path}
      // libgig switches to 64-bit chunk sizes from the size of the file alone
      , m_offsetSize{(m_in.size() >> 32) ? 8u : 4u}
  {
    require(m_in.contains(0, 8 + m_offsetSize), "no RIFF header");
    // 'RIFX' files are big-endian
    require(m_in.u32(0) == cc("RIFF"), "not RIFF");
    const uint64_t size = chunkSize(4);
    require(size >= 4, "RIFF without a form type");
    m_root.id = cc("RIFF");
    m_root.listType = m_in.u32(4 + m_offsetSize);
    m_root.size = size - 4;
    m_root.body = 8 + m_offsetSize;
  }

  const Chunk& root() const noexcept { return m_root; }

  // Whether RIFF::File opens the file at all: it can be read and starts
  // with a RIFF tag
  static bool opens(const QString& path)
  {
    QFile f{path};
    char tag[4]{};
    return f.open(QIODevice::ReadOnly) && f.read(tag, 4) == 4
           && (std::memcmp(tag, "RIFF", 4) == 0 || std::memcmp(tag, "RIFX", 4) == 0);
  }

  void setStreaming(bool streaming) noexcept { m_in.setStreaming(streaming); }

  // RIFF::List::LoadSubChunks(). A sub-chunk keeps the size it declares even
  // when that runs past its list; only the walk is clamped to the list.
  Chunks children(const Chunk& list)
  {
    Chunks out;
    const uint64_t header = 4 + m_offsetSize;
    uint64_t pos = 0;
    while(list.size - pos >= header)
    {
      require(out.size() < maxSubChunks, "too many sub-chunks");
      require(pos <= m_in.size(), "sub-chunk header outside the file");
      const uint64_t at = list.body + pos;
      require(m_in.contains(at, header), "sub-chunk header outside the file");

      Chunk c;
      c.id = m_in.u32(at);
      c.size = chunkSize(at + 4);
      uint64_t advance = header;
      if(c.id == cc("LIST"))
      {
        require(c.size >= 4, "list without a type");
        require(m_in.contains(at + header, 4), "list type outside the file");
        c.listType = m_in.u32(at + header);
        c.size -= 4;
        c.body = at + header + 4;
        advance += 4;
      }
      else
      {
        c.body = at + header;
      }
      require(c.size <= UINT64_MAX - advance, "chunk size overflow");
      advance += c.size;

      pos = advance > list.size - pos ? list.size : pos + advance;
      if(pos % 2 != 0)
        pos = std::min(pos + 1, list.size);
      out.push_back(c);
    }
    return out;
  }

  // Whether libgig can read these bytes of the chunk: inside the size the
  // chunk declares, and inside the file.
  bool has(const Chunk& c, uint64_t offset, uint64_t n) const noexcept
  {
    return offset <= c.size && n <= c.size - offset && offset <= m_in.size()
           && m_in.contains(c.body + offset, n);
  }
  void need(const Chunk& c, uint64_t offset, uint64_t n, const char* why) const
  {
    require(has(c, offset, n), why);
  }
  void needAll(const Chunk& c, const char* why) const { need(c, 0, c.size, why); }

  uint8_t u8(const Chunk& c, uint64_t offset, const char* why)
  {
    need(c, offset, 1, why);
    return m_in.u8(c.body + offset);
  }
  uint16_t u16(const Chunk& c, uint64_t offset, const char* why)
  {
    need(c, offset, 2, why);
    return m_in.u16(c.body + offset);
  }
  uint32_t u32(const Chunk& c, uint64_t offset, const char* why)
  {
    need(c, offset, 4, why);
    return m_in.u32(c.body + offset);
  }

  std::vector<uint8_t>
  bytes(const Chunk& c, uint64_t offset, uint64_t n, const char* why)
  {
    need(c, offset, n, why);
    std::vector<uint8_t> data(n);
    m_in.read(c.body + offset, data.data(), n);
    return data;
  }

  // libgig's LoadString(): the whole chunk, up to its first NUL
  std::string text(const Chunk& c)
  {
    require(c.size <= maxTextSize, "oversized text chunk");
    needAll(c, "text chunk outside the file");
    std::string s(c.size, '\0');
    m_in.read(c.body, s.data(), c.size);
    s.resize(std::find(s.begin(), s.end(), '\0') - s.begin());
    return s;
  }

private:
  uint64_t chunkSize(uint64_t at)
  {
    return m_offsetSize == 4 ? m_in.u32(at) : m_in.u64(at);
  }

  FileReader m_in;
  unsigned m_offsetSize;
  Chunk m_root;
};

//////////////////////////////////////////////////////////////////////////////
// DLS and gig
//////////////////////////////////////////////////////////////////////////////

// The INFO fields DLS::Info loads from every resource
constexpr uint32_t dlsInfoFields[] = {
    cc("INAM"), cc("IARL"), cc("ICRD"), cc("ICMT"), cc("IPRD"), cc("ICOP"),
    cc("IART"), cc("IGNR"), cc("IKEY"), cc("IENG"), cc("ITCH"), cc("ISFT"),
    cc("IMED"), cc("ISRC"), cc("ISRF"), cc("ICMS"), cc("ISBJ")};

// DLS::Resource: the file, an instrument, a region or a sample. Returns its
// INAM.
std::string resource(Riff& riff, const Chunks& chunks)
{
  std::string name;
  if(const auto* info = firstList(chunks, cc("INFO")))
  {
    const auto fields = riff.children(*info);
    for(const uint32_t id : dlsInfoFields)
    {
      const auto* field = lastChunk(fields, id);
      if(!field)
        continue;
      if(id == cc("INAM"))
      {
        name = riff.text(*field);
      }
      else
      {
        require(field->size <= maxTextSize, "oversized text chunk");
        riff.needAll(*field, "text chunk outside the file");
      }
    }
  }
  if(const auto* dlid = lastChunk(chunks, cc("dlid")))
    riff.need(*dlid, 0, 16, "short dlid");
  return name;
}

// DLS::Sampler: the 'wsmp' chunk of a region, a dimension region or a sample
void sampler(Riff& riff, const Chunks& chunks)
{
  const auto* wsmp = lastChunk(chunks, cc("wsmp"));
  if(!wsmp)
    return;
  riff.need(*wsmp, 0, 20, "short wsmp");
  const uint32_t headerSize = riff.u32(*wsmp, 0, "short wsmp");
  const uint32_t loops = riff.u32(*wsmp, 16, "short wsmp");
  if(loops)
  {
    require(loops <= maxSampleLoops, "too many sample loops");
    riff.need(*wsmp, headerSize, uint64_t(loops) * 16, "short wsmp loops");
  }
}

struct DlsHeader
{
  Chunks chunks;
  bool gig3{};
};

// DLS::File's constructor
DlsHeader dlsHeader(Riff& riff)
{
  DlsHeader h;
  h.chunks = riff.children(riff.root());
  resource(riff, h.chunks);

  if(const auto* vers = lastChunk(h.chunks, cc("vers")))
  {
    riff.need(*vers, 0, 8, "short vers");
    h.gig3 = riff.u16(*vers, 2, "short vers") > 2;
  }

  const auto* colh = lastChunk(h.chunks, cc("colh"));
  require(colh, "no colh");
  riff.need(*colh, 0, 4, "short colh");

  if(const auto* ptbl = lastChunk(h.chunks, cc("ptbl")))
  {
    riff.need(*ptbl, 0, 8, "short ptbl");
    const uint32_t headerSize = riff.u32(*ptbl, 0, "short ptbl");
    const uint32_t count = riff.u32(*ptbl, 4, "short ptbl");
    require(count <= maxWavePoolEntries, "too many wave pool entries");
    require(headerSize <= ptbl->size, "short ptbl");
    // 64-bit entries when they fill the chunk exactly, 32-bit ones otherwise
    const uint64_t rest = ptbl->size - headerSize;
    const uint64_t entry = rest == uint64_t(count) * 8 ? 8 : 4;
    riff.need(*ptbl, headerSize, count * entry, "short ptbl");
  }
  return h;
}

struct MidiLocale
{
  uint32_t bank{};
  uint32_t program{};
};

// DLS::Instrument's own part: the 'insh' chunk
MidiLocale instrumentHeader(Riff& riff, const Chunks& ins)
{
  MidiLocale locale;
  if(const auto* insh = lastChunk(ins, cc("insh")))
  {
    riff.need(*insh, 0, 12, "short insh");
    locale.bank = riff.u32(*insh, 4, "short insh");
    locale.program = riff.u32(*insh, 8, "short insh");
  }
  return locale;
}

std::vector<BankEntryName> dlsNames(Riff& riff)
{
  const auto file = dlsHeader(riff);

  std::vector<BankEntryName> names;
  const auto* lins = firstList(file.chunks, cc("lins"));
  if(!lins)
    return names;
  for(const auto& ins : riff.children(*lins))
  {
    if(!ins.isList(cc("ins ")))
      continue;
    const auto chunks = riff.children(ins);
    // An instrument guarded by a DLS2 condition is listed or not depending
    // on the evaluation of that expression
    require(!lastChunk(chunks, cc("cdl ")), "conditional instrument");

    BankEntryName e;
    e.name = resource(riff, chunks);
    const auto locale = instrumentHeader(riff, chunks);
    e.dlsBank = locale.bank;
    e.dlsProgram = locale.program;
    names.push_back(std::move(e));
  }
  return names;
}

// gig::Instrument's MIDI rules, at the end of the '3ewg' chunk
void gigInstrumentSettings(Riff& riff, const Chunk& ewg)
{
  riff.need(ewg, 0, 12, "short 3ewg");
  if(ewg.size <= 32)
    return;
  const uint8_t id1 = riff.u8(ewg, 32, "short 3ewg");
  const uint8_t id2 = riff.u8(ewg, 33, "short 3ewg");
  if(id2 != 16)
    return;
  switch(id1)
  {
    case 4: // controller triggers
    {
      const uint8_t triggers = riff.u8(ewg, 36, "short trigger rule");
      require(triggers <= 32, "too many triggers");
      riff.need(ewg, 40, 1, "short trigger rule");
      riff.need(ewg, 46, triggers * 8u, "short trigger rule");
      break;
    }
    case 0: // legato
      riff.need(ewg, 36, 1, "short legato rule");
      riff.need(ewg, 40, 13, "short legato rule");
      riff.need(ewg, 64, 3, "short legato rule");
      break;
    case 3: // alternator
    {
      riff.need(ewg, 36, 12, "short alternator rule");
      const auto articulations
          = std::min<unsigned>(riff.u8(ewg, 36, "short alternator rule"), 32);
      const auto patterns
          = std::min<unsigned>(riff.u8(ewg, 38, "short alternator rule"), 32);
      riff.need(ewg, 48, articulations * 32u, "short alternator rule");
      riff.need(ewg, 1072, patterns * 49u, "short alternator rule");
      break;
    }
    default:
      break;
  }
}

// The velocity split points of a dimension region
struct VelocityLimits
{
  // gig 3: per dimension; those of a region without '3ewa' chunk
  uint8_t dimensionUpper[8]{127, 127, 127, 127, 127, 127, 127, 127};
  // gig 2
  uint8_t velocityUpper{0};
};

// gig::DimensionRegion
VelocityLimits gigDimensionRegion(Riff& riff, const Chunk& ewl)
{
  VelocityLimits limits;
  const auto chunks = riff.children(ewl);
  sampler(riff, chunks);
  if(const auto* ewa = lastChunk(chunks, cc("3ewa")))
  {
    riff.need(*ewa, 0, 140, "short 3ewa");
    limits.velocityUpper = riff.u8(*ewa, 124, "short 3ewa");
    // The per-dimension limits were appended by gig 3
    if(ewa->size >= 148)
    {
      const auto upper = riff.bytes(*ewa, 140, 8, "short 3ewa");
      std::copy(upper.begin(), upper.end(), limits.dimensionUpper);
    }
    else
    {
      std::fill(std::begin(limits.dimensionUpper), std::end(limits.dimensionUpper), 0);
    }
  }
  if(const auto* lsde = lastChunk(chunks, cc("LSDE")))
  {
    riff.need(*lsde, 0, 2, "short LSDE");
    if(lsde->size > 3)
      riff.need(*lsde, 3, 1, "short LSDE");
    if(lsde->size > 4)
      riff.need(*lsde, 4, 24, "short LSDE");
  }
  return limits;
}

struct GigDimension
{
  uint8_t type{};
  uint8_t bits{};
  uint8_t zones{};
};

// gig::Region::UpdateVelocityTable(): for a region split by velocity, libgig
// fills a 128-entry table per case of the other dimensions, walking the
// dimension regions as the dimension definitions lay them out. It trusts
// both: this follows the same walk and leaves to libgig a region whose
// definitions lead outside of its dimension regions, or whose split points
// lie outside of the table.
void gigVelocityTables(
    const GigDimension (&dimensions)[8], int active,
    const std::vector<VelocityLimits>& regions, uint32_t count)
{
  constexpr uint8_t velocity = 0x82;
  int veldim = -1;
  for(int i = 0; i < active; i++)
  {
    if(dimensions[i].type == velocity)
    {
      veldim = i;
      break;
    }
  }
  if(veldim == -1)
    return;

  int step = 1;
  for(int i = 0; i < veldim; i++)
    step <<= dimensions[i].bits;
  const int skipVelocity = (step << dimensions[veldim].bits) - step;

  int cases[8]{};
  for(int i = 0; i < int(count); i++)
  {
    const int end = i + step * dimensions[veldim].zones;
    const auto& first = regions[i];
    if(first.dimensionUpper[veldim] || first.velocityUpper)
    {
      const bool gig3 = first.dimensionUpper[veldim];
      for(int k = i; k < end; k += step)
      {
        require(k < int(regions.size()), "velocity zones without dimension region");
        const uint8_t upper
            = gig3 ? regions[k].dimensionUpper[veldim] : regions[k].velocityUpper;
        require(upper <= 127, "velocity split point above 127");
      }
    }

    int j = 0;
    int shift = 0;
    for(; j < active; j++)
    {
      if(j == veldim)
      {
        i += skipVelocity;
      }
      else
      {
        cases[j]++;
        if(cases[j] < dimensions[j].zones)
          break;
        cases[j] = 0;
        const int unused = (1 << dimensions[j].bits) - dimensions[j].zones;
        require(unused >= 0, "more zones than dimension bits");
        i += unused << shift;
      }
      shift += dimensions[j].bits;
    }
    if(j == active)
      break;
  }
}

// gig::Region
void gigRegion(Riff& riff, const Chunk& rgn, bool gig3)
{
  const auto chunks = riff.children(rgn);
  resource(riff, chunks);
  sampler(riff, chunks);
  if(const auto* rgnh = lastChunk(chunks, cc("rgnh")))
    riff.need(*rgnh, 0, 12, "short rgnh");
  if(const auto* wlnk = lastChunk(chunks, cc("wlnk")))
    riff.need(*wlnk, 0, 12, "short wlnk");

  std::vector<VelocityLimits> dimensionRegions;
  if(const auto* prg = firstList(chunks, cc("3prg")))
  {
    for(const auto& ewl : riff.children(*prg))
    {
      if(!ewl.isList(cc("3ewl")))
        continue;
      require(dimensionRegions.size() < 256, "too many dimension regions");
      dimensionRegions.push_back(gigDimensionRegion(riff, ewl));
    }
    require(!dimensionRegions.empty(), "3prg without dimension regions");
  }

  // Without a '3lnk' chunk, or with one that counts no dimension region,
  // libgig makes a region up
  const auto* lnk = lastChunk(chunks, cc("3lnk"));
  require(lnk, "region without 3lnk");
  const uint32_t count = riff.u32(*lnk, 0, "short 3lnk");
  require(count >= 1, "3lnk without dimension regions");
  require(
      count <= dimensionRegions.size(), "3lnk counts missing dimension regions");
  riff.need(*lnk, 0, (gig3 ? 68u : 44u) + 4u * count, "short 3lnk");

  // The dimension definitions: five before gig 3, eight since. The unused
  // ones are left in place, and counted out of the number of dimensions.
  GigDimension dimensions[8];
  int active = 0;
  unsigned bits = 0;
  for(unsigned i = 0; i < (gig3 ? 8u : 5u); i++)
  {
    const auto definition = riff.bytes(*lnk, 4 + 8 * i, 5, "short 3lnk");
    if(definition[0] == 0)
      continue;
    auto& d = dimensions[i];
    d.type = definition[0];
    d.bits = definition[1];
    bits += d.bits;
    // A region has at most 256 dimension regions
    require(bits <= 8, "too many dimension bits");
    d.zones = definition[4] ? definition[4] : uint8_t(1u << d.bits);
    active++;
  }
  gigVelocityTables(dimensions, active, dimensionRegions, count);
}

// gig::Instrument's script slots
void gigScripts(Riff& riff, const Chunks& chunks)
{
  require(!lastChunk(chunks, cc("SCPV")), "script variables");
  const auto* scsl = lastChunk(chunks, cc("SCSL"));
  if(!scsl)
    return;
  riff.need(*scsl, 0, 8, "short SCSL");
  const uint32_t headerSize = riff.u32(*scsl, 0, "short SCSL");
  const uint32_t slotCount = riff.u32(*scsl, 4, "short SCSL");
  if(!slotCount)
    return;
  require(slotCount <= maxScriptSlots, "too many script slots");
  require(riff.u32(*scsl, 8, "short SCSL") == 8, "extended script slots");
  riff.need(*scsl, headerSize, slotCount * 8u, "short SCSL");
}

// gig::Sample::ScanCompressedSample(): libgig goes through the frames of a
// compressed sample to count its sample points, from the compression mode
// each frame starts with (one per channel)
void gigCompressedFrames(Riff& riff, const Chunk& data, bool stereo)
{
  static constexpr uint32_t bytesPerFrame[] = {4096, 2052, 768, 524, 396, 268};

  riff.setStreaming(true);
  uint64_t pos = 0;
  for(;;)
  {
    uint64_t frameSize = 0;
    for(int channel = 0; channel < (stereo ? 2 : 1); channel++)
    {
      const uint8_t mode = riff.u8(data, pos + channel, "compressed data ends early");
      require(mode <= 5, "unknown compression mode");
      frameSize += bytesPerFrame[mode];
    }
    pos += stereo ? 2 : 1;
    if(data.size - pos <= frameSize)
      break;
    pos += frameSize;
  }
  riff.setStreaming(false);
}

// gig::Sample
void gigSample(Riff& riff, const Chunk& wave)
{
  const auto chunks = riff.children(wave);
  resource(riff, chunks);

  // What a sample without 'fmt ' chunk is taken for
  unsigned channels = 1;
  unsigned bitDepth = 16;
  const auto* data = lastChunk(chunks, cc("data"));
  if(const auto* fmt = lastChunk(chunks, cc("fmt ")))
  {
    riff.need(*fmt, 0, 14, "short fmt");
    channels = riff.u16(*fmt, 2, "short fmt");
    if(riff.u16(*fmt, 0, "short fmt") == 1) // PCM
    {
      bitDepth = riff.u16(*fmt, 14, "short fmt");
      // libgig divides the size of the sample data by this
      require(!data || (bitDepth / 8) * channels != 0, "empty sample frame");
    }
    else
    {
      bitDepth = 0;
    }
  }
  require(bitDepth <= 24, "more than 24 bits");

  sampler(riff, chunks);
  if(const auto* gix = lastChunk(chunks, cc("3gix")))
    riff.need(*gix, 0, 2, "short 3gix");
  if(const auto* smpl = lastChunk(chunks, cc("smpl")))
    riff.need(*smpl, 0, 60, "short smpl");

  if(const auto* ewav = lastChunk(chunks, cc("ewav")))
  {
    const uint32_t version = riff.u32(*ewav, 0, "short ewav");
    if(version > 2 && bitDepth == 24)
    {
      riff.need(*ewav, 4, 4, "short ewav");
      riff.need(*ewav, channels == 2 ? 84 : 64, 4, "short ewav");
    }
    require(data, "compressed sample without data");
    gigCompressedFrames(riff, *data, channels == 2);
  }
}

// The samples of one file of a gig bank
void gigWavePool(Riff& riff, const Chunks& chunks)
{
  if(const auto* wvpl = firstList(chunks, cc("wvpl")))
    for(const auto& wave : riff.children(*wvpl))
      if(wave.isList(cc("wave")))
        gigSample(riff, wave);
}

// libgig's parentPath(), pathWithoutExtension() and concatPath(), from which
// it derives where the extension files of a bank are
#if defined(_WIN32)
constexpr char pathSeparator = '\\';
#else
constexpr char pathSeparator = '/';
#endif

std::string parentPath(std::string path)
{
  if(path.empty())
    return path;
  if(path.back() == pathSeparator)
  {
    const auto last = path.find_last_not_of(pathSeparator);
    path = last == std::string::npos ? std::string{} : path.substr(0, last + 1);
  }
  if(path.empty())
    return std::string(1, pathSeparator);
#if defined(_WIN32)
  if(path.length() == 2 && path[1] == ':')
    return path;
#endif
  const auto pos = path.find_last_of(pathSeparator);
  if(pos == std::string::npos)
    return {};
  if(pos == 0)
    return std::string(1, pathSeparator);
  return path.substr(0, pos);
}

std::string pathWithoutExtension(const std::string& path)
{
  const auto separator = path.find_last_of(pathSeparator);
  const auto base = separator == std::string::npos ? 0 : separator + 1;
  const auto dot = path.find_last_of('.');
  return dot != std::string::npos && dot > base ? path.substr(0, dot) : path;
}

std::string concatPath(const std::string& a, const std::string& b)
{
  return !a.empty() && a.back() != pathSeparator && !b.empty()
                 && b.front() != pathSeparator
             ? a + pathSeparator + b
             : a + b;
}

// gig::File::LoadSamples(): the samples of a large bank continue in
// extension files (.gx01, .gx02...) listed in the 'xfil' chunk, which
// libgig opens and rejects the bank without; a GigaPulse companion (.gx99),
// announced by the 'doxf' chunk, is used when it is there and matches.
void gigExtensionFiles(Riff& riff, const Chunks& chunks, const std::string& path)
{
  if(const auto* xfil = lastChunk(chunks, cc("xfil")))
  {
    const uint32_t count = riff.u32(*xfil, 0, "short xfil");
    require(count <= 99, "too many extension files");
    uint64_t pos = 4;
    for(uint32_t i = 0; i < count; i++)
    {
      auto field = riff.bytes(*xfil, pos, 128, "short xfil");
      pos += 128;
      const std::string name(
          field.begin(), std::find(field.begin(), field.end(), uint8_t{0}));

      Riff extension{QString::fromStdString(concatPath(parentPath(path), name))};
      const auto extensionChunks = extension.children(extension.root());
      // The ID the bank expects is only there to be read when the
      // extension file has one
      if(const auto* dlid = lastChunk(extensionChunks, cc("dlid")))
      {
        const auto expected = riff.bytes(*xfil, pos, 16, "short xfil");
        pos += 16;
        const auto found = extension.bytes(*dlid, 0, 16, "short dlid");
        require(expected == found, "extension file of another bank");
      }
      gigWavePool(extension, extensionChunks);
    }
  }

  if(const auto* doxf = lastChunk(chunks, cc("doxf")))
  {
    const auto companion
        = QString::fromStdString(pathWithoutExtension(path) + ".gx99");
    if(Riff::opens(companion))
    {
      Riff extension{companion};
      const auto extensionChunks = extension.children(extension.root());
      bool used = true;
      if(const auto* dlid = lastChunk(extensionChunks, cc("dlid")))
      {
        // Past an unused word and the file name
        if(riff.has(*doxf, 132, 8))
        {
          const auto expected = riff.bytes(*doxf, 132, 16, "short doxf");
          const auto found = extension.bytes(*dlid, 0, 16, "short dlid");
          used = expected == found;
        }
        else
        {
          used = false;
        }
      }
      if(used)
        gigWavePool(extension, extensionChunks);
    }
  }
}

std::vector<BankEntryName> gigNames(Riff& riff, const QString& filePath)
{
  const auto file = dlsHeader(riff);

  std::vector<BankEntryName> names;
  if(const auto* lins = firstList(file.chunks, cc("lins")))
  {
    for(const auto& ins : riff.children(*lins))
    {
      if(!ins.isList(cc("ins ")))
        continue;
      const auto chunks = riff.children(ins);

      BankEntryName e;
      e.name = resource(riff, chunks);
      instrumentHeader(riff, chunks);

      if(const auto* lart = firstList(chunks, cc("lart")))
      {
        const auto articulation = riff.children(*lart);
        if(const auto* ewg = lastChunk(articulation, cc("3ewg")))
          gigInstrumentSettings(riff, *ewg);
      }

      if(const auto* lrgn = firstList(chunks, cc("lrgn")))
        for(const auto& rgn : riff.children(*lrgn))
          if(rgn.isList(cc("rgn ")))
            gigRegion(riff, rgn, file.gig3);

      if(const auto* scripts = firstList(chunks, cc("3LS ")))
        gigScripts(riff, riff.children(*scripts));

      names.push_back(std::move(e));
    }
  }

  // The regions resolve their samples, which makes libgig load the sample
  // groups and the header of every sample of the file
  if(const auto* gri = firstList(file.chunks, cc("3gri")))
  {
    const auto groups = riff.children(*gri);
    if(const auto* gnl = firstList(groups, cc("3gnl")))
    {
      for(const auto& gnm : riff.children(*gnl))
      {
        if(gnm.id != cc("3gnm"))
          continue;
        require(gnm.size >= 1 && gnm.size <= maxTextSize, "odd group name");
        riff.needAll(gnm, "group name outside the file");
      }
    }
  }

  gigExtensionFiles(riff, file.chunks, filePath.toStdString());
  gigWavePool(riff, file.chunks);

  return names;
}

//////////////////////////////////////////////////////////////////////////////
// SoundFont 2
//////////////////////////////////////////////////////////////////////////////

struct Sf2Table
{
  std::vector<uint8_t> data; // empty unless asked for
  uint32_t count{};
};

// One of the fixed-record chunks of the 'pdta' list, under the conditions
// sf2::File puts on its size; libgig reads all the records.
Sf2Table sf2Table(
    Riff& riff, const Chunks& pdta, uint32_t id, uint32_t recordSize,
    uint32_t minSize, bool wholeRecords, bool load)
{
  const auto* ck = lastChunk(pdta, id);
  require(ck, "missing pdta chunk");
  require(ck->size >= minSize && ck->size <= maxTableSize, "bad pdta chunk size");
  require(!wholeRecords || ck->size % recordSize == 0, "bad pdta chunk size");

  Sf2Table t;
  t.count = uint32_t(ck->size / recordSize);
  const uint64_t used = uint64_t(t.count) * recordSize;
  if(load)
    t.data = riff.bytes(*ck, 0, used, "pdta chunk outside the file");
  else
    riff.need(*ck, 0, used, "pdta chunk outside the file");
  return t;
}

uint16_t le16(const std::vector<uint8_t>& data, std::size_t at) noexcept
{
  return uint16_t(data[at] | data[at + 1] << 8);
}

// sf2::LoadString() on a 20-byte name field: up to the first NUL that is
// not the very first byte, the whole field when there is none.
std::string sf2Name(const uint8_t* field)
{
  std::size_t len = 20;
  for(std::size_t i = 1; i < 20; i++)
  {
    if(field[i] == 0)
    {
      len = i;
      break;
    }
  }
  return std::string(reinterpret_cast<const char*>(field), len);
}

std::vector<BankEntryName> sf2Names(Riff& riff)
{
  require(riff.root().listType == cc("sfbk"), "not sfbk");
  const auto top = riff.children(riff.root());

  // sf2::Info
  const auto* infoList = firstList(top, cc("INFO"));
  require(infoList, "no INFO");
  const auto info = riff.children(*infoList);
  const auto* ifil = lastChunk(info, cc("ifil"));
  require(ifil, "no ifil");
  riff.need(*ifil, 0, 4, "short ifil");
  require(riff.u16(*ifil, 0, "short ifil") == 2, "not version 2");
  for(const uint32_t id :
      {cc("isng"), cc("INAM"), cc("irom"), cc("ICRD"), cc("IENG"), cc("IPRD"),
       cc("ICOP"), cc("ICMT"), cc("ISFT")})
  {
    if(const auto* field = lastChunk(info, id))
    {
      require(field->size <= maxTextSize, "oversized text chunk");
      riff.needAll(*field, "text chunk outside the file");
    }
  }
  if(const auto* iver = lastChunk(info, cc("iver")))
    riff.need(*iver, 0, 4, "short iver");

  const auto* sdta = firstList(top, cc("sdta"));
  require(sdta, "no sdta");
  riff.children(*sdta);

  const auto* pdtaList = firstList(top, cc("pdta"));
  require(pdtaList, "no pdta");
  const auto pdta = riff.children(*pdtaList);

  // In the order sf2::File reads them
  const auto phdr = sf2Table(riff, pdta, cc("phdr"), 38, 38, false, true);
  const auto pbag = sf2Table(riff, pdta, cc("pbag"), 4, 4, true, true);
  sf2Table(riff, pdta, cc("pmod"), 10, 0, true, false);
  const auto pgen = sf2Table(riff, pdta, cc("pgen"), 4, 4, true, false);
  const auto inst = sf2Table(riff, pdta, cc("inst"), 22, 44, true, true);
  const auto ibag = sf2Table(riff, pdta, cc("ibag"), 4, 4, true, true);
  const auto imod = sf2Table(riff, pdta, cc("imod"), 10, 0, true, false);
  const auto igen = sf2Table(riff, pdta, cc("igen"), 4, 4, true, false);
  sf2Table(riff, pdta, cc("shdr"), 46, 0, true, false);

  // The zones of every instrument, then of every preset, are delimited by
  // indices into the next table; libgig rejects the file on the first range
  // that does not fit. The last record of each table only ends the one
  // before it.
  for(uint32_t i = 0; i + 1 < inst.count; i++)
  {
    const uint32_t first = le16(inst.data, i * 22 + 20);
    const uint32_t last = le16(inst.data, (i + 1) * 22 + 20);
    require(first <= last && last < ibag.count, "invalid InstBagNdx");
    for(uint32_t bag = first; bag < last; bag++)
    {
      const uint32_t gen1 = le16(ibag.data, bag * 4);
      const uint32_t gen2 = le16(ibag.data, (bag + 1) * 4);
      require(gen1 <= gen2 && gen2 < igen.count, "invalid InstGenNdx");
      const uint32_t mod1 = le16(ibag.data, bag * 4 + 2);
      const uint32_t mod2 = le16(ibag.data, (bag + 1) * 4 + 2);
      require(mod1 <= mod2 && mod2 < imod.count, "invalid InstModNdx");
    }
  }
  for(uint32_t i = 0; i + 1 < phdr.count; i++)
  {
    const uint32_t first = le16(phdr.data, i * 38 + 24);
    const uint32_t last = le16(phdr.data, (i + 1) * 38 + 24);
    require(first <= last && last < pbag.count, "invalid PresetBagNdx");
    for(uint32_t bag = first; bag < last; bag++)
    {
      const uint32_t gen1 = le16(pbag.data, bag * 4);
      const uint32_t gen2 = le16(pbag.data, (bag + 1) * 4);
      require(gen1 <= gen2 && gen2 < pgen.count, "invalid PresetGenNdx");
    }
  }

  // The last preset record is the terminal one ("EOP")
  std::vector<BankEntryName> names;
  names.reserve(phdr.count - 1);
  for(uint32_t i = 0; i + 1 < phdr.count; i++)
    names.push_back({sf2Name(phdr.data.data() + i * 38), 0, 0});
  return names;
}
}

std::optional<std::vector<BankEntryName>>
readBankNames(const QString& filePath, BankKind kind, const char** whyNot)
{
  if(whyNot)
    *whyNot = nullptr;
  try
  {
    Riff riff{filePath};
    switch(kind)
    {
      case BankKind::Gig:
        return gigNames(riff, filePath);
      case BankKind::Dls:
        return dlsNames(riff);
      case BankKind::Sf2:
        return sf2Names(riff);
    }
  }
  catch(const Unsure& e)
  {
    if(whyNot)
      *whyNot = e.why;
  }
  catch(const std::exception&)
  {
    if(whyNot)
      *whyNot = "exception";
  }
  return std::nullopt;
}
}
