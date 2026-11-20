// The library scan lists the instruments of gig, dls and sf2 banks without
// parsing them (listInstrumentNames); these tests hold it to the result of
// the libgig-based listInstruments, on synthetic files built here and, when
// DEUTERIUM_BANK_ROOTS names folders (separated like PATH), on every bank
// found under them:
//
//   DEUTERIUM_BANK_ROOTS=~/samples Deuterium_BankNamesTest "bank names: collection"
#include <Deuterium/GigSampler/BankNames.hpp>
#include <Deuterium/GigSampler/GigLoader.hpp>
#include <Deuterium/GigSampler/Library.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <DLS.h>
#include <gig.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace Deuterium::Gig;

namespace
{
QString tmp(const QString& name)
{
  static QTemporaryDir dir;
  return dir.filePath(name);
}

QString write(const QString& name, const std::string& bytes)
{
  const auto path = tmp(name);
  QFile f{path};
  REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
  f.close();
  return path;
}

std::string read(const QString& path)
{
  QFile f{path};
  REQUIRE(f.open(QIODevice::ReadOnly));
  return f.readAll().toStdString();
}

/////////////////////////////
// RIFF building blocks
/////////////////////////////

std::string le16(uint32_t v)
{
  return {char(v & 0xFF), char((v >> 8) & 0xFF)};
}
std::string le32(uint32_t v)
{
  return le16(v & 0xFFFF) + le16(v >> 16);
}
std::string zeros(std::size_t n)
{
  return std::string(n, '\0');
}
std::string join(const std::vector<std::string>& parts)
{
  std::string s;
  for(const auto& p : parts)
    s += p;
  return s;
}

// A chunk declaring `size` bytes, whatever its body really holds
std::string chunkSized(const char* id, uint32_t size, const std::string& body)
{
  return std::string(id, 4) + le32(size) + body;
}
std::string chunk(const char* id, const std::string& body)
{
  return chunkSized(id, uint32_t(body.size()), body + zeros(body.size() % 2));
}
std::string list(const char* type, const std::vector<std::string>& children)
{
  return chunk("LIST", std::string(type, 4) + join(children));
}
std::string riff(const char* type, const std::vector<std::string>& children)
{
  return chunk("RIFF", std::string(type, 4) + join(children));
}
std::string info(const std::string& name)
{
  return list("INFO", {chunk("INAM", name)});
}

/////////////////////////////
// SoundFont 2
/////////////////////////////

std::string field20(std::string name)
{
  name.resize(20, '\0');
  return name;
}

// One instrument of one sample; each preset has a single zone playing it.
// The names are the raw 20-byte fields (shorter ones are padded with NUL).
struct Sf2
{
  std::vector<std::string> presets;
  std::string infoList = list(
      "INFO", {chunk("ifil", le16(2) + le16(1)), chunk("isng", "EMU8000" + zeros(1)),
               chunk("INAM", "Bank" + zeros(1))});
  // Replacements for single pdta chunks, by ID
  std::map<std::string, std::string> pdtaOverride;

  std::string build() const
  {
    const auto n = uint32_t(presets.size());
    std::string phdr, pbag, pgen;
    for(uint32_t i = 0; i < n; i++)
    {
      phdr += field20(presets[i]) + le16(i) + le16(0) + le16(i) + zeros(12);
      pbag += le16(i) + le16(0);
      pgen += le16(41) + le16(0); // instrument 0
    }
    phdr += field20("EOP") + le16(0) + le16(0) + le16(n) + zeros(12);
    pbag += le16(n) + le16(0);
    pgen += le16(0) + le16(0);

    const std::string inst = field20("Inst") + le16(0) + field20("EOI") + le16(1);
    const std::string ibag = le16(0) + le16(0) + le16(1) + le16(0);
    const std::string igen = le16(53) + le16(0) + le16(0) + le16(0); // sample 0
    const std::string shdr = field20("Sample") + le32(0) + le32(8) + le32(0)
                             + le32(0) + le32(44100) + std::string{char(60), 0}
                             + le16(0) + le16(1) + field20("EOS") + zeros(26);

    std::vector<std::string> pdta;
    const auto add = [&](const char* id, const std::string& body) {
      if(auto it = pdtaOverride.find(id); it != pdtaOverride.end())
        pdta.push_back(it->second);
      else
        pdta.push_back(chunk(id, body));
    };
    add("phdr", phdr);
    add("pbag", pbag);
    add("pmod", zeros(10));
    add("pgen", pgen);
    add("inst", inst);
    add("ibag", ibag);
    add("imod", zeros(10));
    add("igen", igen);
    add("shdr", shdr);

    return riff(
        "sfbk",
        {infoList, list("sdta", {chunk("smpl", zeros(64))}), list("pdta", pdta)});
  }
};

/////////////////////////////
// DLS
/////////////////////////////

std::string dlsRegion()
{
  return list(
      "rgn ",
      {chunk("rgnh", le16(0) + le16(127) + le16(0) + le16(127) + le16(0) + le16(0)),
       chunk("wsmp", le32(20) + le16(60) + le16(0) + le32(0) + le32(0) + le32(0)),
       chunk("wlnk", le16(0) + le16(0) + le32(1) + le32(0))});
}

std::string dlsInsh(uint32_t bank, uint32_t program)
{
  return chunk("insh", le32(1) + le32(bank) + le32(program));
}

// An instrument with its header, one region, and whatever comes in `extra`
// (its INFO list, usually)
std::string dlsInstrument(uint32_t bank, uint32_t program, const std::string& extra)
{
  return list("ins ", {dlsInsh(bank, program), list("lrgn", {dlsRegion()}), extra});
}

std::string dlsWavePool()
{
  return list(
      "wvpl",
      {list(
          "wave", {chunk(
                       "fmt ", le16(1) + le16(1) + le32(44100) + le32(88200)
                                   + le16(2) + le16(16)),
                   chunk("data", zeros(16))})});
}

std::string dls(const std::vector<std::string>& instruments)
{
  return riff(
      "DLS ",
      {chunk("colh", le32(uint32_t(instruments.size()))),
       chunk("vers", le16(0) + le16(1) + le16(0) + le16(0)),
       list("lins", instruments), chunk("ptbl", le32(8) + le32(1) + le32(0)),
       dlsWavePool(), info("Bank" + zeros(1))});
}

/////////////////////////////
// gig, through libgig's own writer
/////////////////////////////

// velocitySplit: the regions get two velocity zones, split at that velocity
QString makeGig(
    const QString& path, const std::vector<std::string>& names,
    int velocitySplit = 0)
{
  gig::File f;
  f.pInfo->Name = "TestBank";

  gig::Sample* smp = f.AddSample();
  smp->pInfo->Name = "TestSample";
  smp->Channels = 1;
  smp->BitDepth = 16;
  smp->FrameSize = 2;
  smp->SamplesPerSecond = 44100;
  smp->MIDIUnityNote = 60;
  smp->Resize(32);

  for(const auto& name : names)
  {
    gig::Instrument* ins = f.AddInstrument();
    ins->pInfo->Name = name;
    gig::Region* rgn = ins->AddRegion();
    rgn->SetKeyRange(36, 48);
    rgn->SetSample(smp);
    rgn->pDimensionRegions[0]->pSample = smp;

    if(velocitySplit > 0)
    {
      gig::dimension_def_t def{};
      def.dimension = gig::dimension_velocity;
      def.bits = 1;
      def.zones = 2;
      rgn->AddDimension(&def);
      for(uint32_t z = 0; z < rgn->DimensionRegions; z++)
      {
        auto* dr = rgn->pDimensionRegions[z];
        dr->pSample = smp;
        dr->DimensionUpperLimits[0] = z == 0 ? velocitySplit : 127;
        dr->VelocityUpperLimit = dr->DimensionUpperLimits[0];
      }
    }
  }
  f.Save(path.toStdString());

  std::vector<int16_t> data(32, 1000);
  smp->SetPos(0);
  smp->Write(data.data(), 32);
  return path;
}

/////////////////////////////
// Comparison
/////////////////////////////

struct Listing
{
  std::vector<std::string> names;     // listInstrumentNames
  std::vector<std::string> reference; // listInstruments
  InstrumentNamesSource source;
};

Listing listBoth(const QString& path)
{
  Listing l;
  l.names = listInstrumentNames(path, &l.source);
  l.reference = listInstruments(path);
  return l;
}

std::string printable(const std::string& s)
{
  std::string out = "\"";
  for(const unsigned char c : s)
  {
    if(c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
    {
      out += char(c);
    }
    else
    {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X", c);
      out += buf;
    }
  }
  return out + "\"";
}

std::string printable(const std::vector<std::string>& names)
{
  std::string out = "[" + std::to_string(names.size()) + "]";
  for(const auto& n : names)
    out += " " + printable(n);
  return out;
}

using Names = std::vector<std::string>;

// The names come straight from the file's chunks and are libgig's
void requireDirect(const QString& path, const Names& expected)
{
  const auto l = listBoth(path);
  INFO(path.toStdString() << ": " << printable(l.names) << " / libgig "
                          << printable(l.reference) << " ("
                          << (l.source.whyNotDirect ? l.source.whyNotDirect : "direct")
                          << ")");
  REQUIRE(l.source.direct);
  REQUIRE(l.names == l.reference);
  REQUIRE(l.names == expected);
}

// The file is left to libgig, whose result is returned
Names requireParsed(const QString& path)
{
  const auto l = listBoth(path);
  INFO(path.toStdString() << ": " << printable(l.names) << " / libgig "
                          << printable(l.reference));
  REQUIRE(!l.source.direct);
  REQUIRE(l.source.whyNotDirect);
  REQUIRE(l.names == l.reference);
  return l.names;
}

BankKind kindOf(const QString& path)
{
  const auto format = formatName(path);
  return format == "SF2" ? BankKind::Sf2 : format == "DLS" ? BankKind::Dls : BankKind::Gig;
}

// For damaged files: either the reader leaves the file to libgig, or it
// gives libgig's names. libgig is only asked about the files the reader
// vouches for: it does not survive every damaged file itself.
bool sameAsLibgig(const QString& path, bool* direct = nullptr)
{
  const bool readable = readBankNames(path, kindOf(path)).has_value();
  if(direct)
    *direct = readable;
  if(!readable)
    return true;

  const auto l = listBoth(path);
  if(l.names != l.reference || !l.source.direct)
    std::printf(
        "MISMATCH %s\n  names:  %s\n  libgig: %s\n", qPrintable(path),
        printable(l.names).c_str(), printable(l.reference).c_str());
  return l.names == l.reference && l.source.direct;
}

bool sameNode(const Library::StagedNode& a, const Library::StagedNode& b)
{
  if(a.data.key != b.data.key || a.data.prettyName != b.data.prettyName
     || a.data.customData != b.data.customData
     || a.children.size() != b.children.size())
    return false;
  for(std::size_t i = 0; i < a.children.size(); i++)
    if(!sameNode(a.children[i], b.children[i]))
      return false;
  return true;
}

bool sameEntry(
    const std::optional<Library::ProcessEntry>& a,
    const std::optional<Library::ProcessEntry>& b)
{
  if(!a || !b)
    return !a && !b;
  return a->rootKey == b->rootKey && a->categoryPath == b->categoryPath
         && sameNode(a->node, b->node);
}

// Every prefix of a valid file, compared with libgig; returns how many of
// them were still read without it
int truncations(const QString& name, const std::string& bytes)
{
  int direct = 0;
  int different = 0;
  for(std::size_t length = 0; length < bytes.size(); length++)
  {
    const auto path = write(name, bytes.substr(0, length));
    bool d{};
    if(!sameAsLibgig(path, &d))
    {
      std::printf("  ^ truncated to %zu of %zu bytes\n", length, bytes.size());
      different++;
    }
    direct += d;
  }
  REQUIRE(different == 0);
  return direct;
}
}

TEST_CASE("bank names: sf2", "[deuterium][banknames]")
{
  Sf2 sf;
  sf.presets = {"Piano", "Strings", "Flute"};
  requireDirect(write("three.sf2", sf.build()), {"Piano", "Strings", "Flute"});

  // The terminal record is the only one: no preset, the file is left out
  sf.presets = {};
  requireDirect(write("empty.sf2", sf.build()), {});
}

TEST_CASE("bank names: sf2 name fields", "[deuterium][banknames]")
{
  Sf2 sf;
  const std::string full(20, 'x');
  sf.presets = {
      full,                                      // no terminator
      std::string("Pad\0junk after", 14),        // garbage after the terminator
      std::string(),                             // all NUL
      std::string("\0Hidden", 7),                // leading NUL
      "Fl\xC3\xBBte \xE2\x99\xAA",               // UTF-8
      "Cor fran\xE7" "ais",                      // Latin-1
      "  padded  ",                              // spaces are kept
  };
  // libgig cuts a name at the first NUL found after its first byte
  requireDirect(
      write("names.sf2", sf.build()),
      {full, "Pad", std::string(1, '\0'), std::string("\0Hidden", 7),
       "Fl\xC3\xBBte \xE2\x99\xAA", "Cor fran\xE7" "ais", "  padded  "});
}

TEST_CASE("bank names: sf2 structure", "[deuterium][banknames]")
{
  Sf2 sf;
  sf.presets = {"A", "B"};
  const auto good = sf.build();

  SECTION("trailing bytes after the RIFF chunk")
  {
    requireDirect(write("trailing.sf2", good + "garbage!"), {"A", "B"});
  }
  SECTION("RIFF size larger than the file")
  {
    auto bytes = good;
    bytes.replace(4, 4, le32(uint32_t(bytes.size()) + 1000));
    CHECK(requireParsed(write("riffsize.sf2", bytes)) == Names{"A", "B"});
  }
  SECTION("a partial preset record at the end of phdr is ignored")
  {
    auto odd = sf;
    const auto phdr = field20("A") + le16(0) + le16(0) + le16(0) + zeros(12)
                      + field20("EOP") + le16(0) + le16(0) + le16(1) + zeros(12);
    odd.pdtaOverride["phdr"] = chunk("phdr", phdr + "xyz");
    requireDirect(write("oddphdr.sf2", odd.build()), {"A"});
  }
  SECTION("unknown and duplicated chunks")
  {
    auto dup = sf;
    dup.infoList = list(
        "INFO",
        {chunk("ifil", le16(1) + le16(0)), chunk("ifil", le16(2) + le16(4)),
         chunk("ICMT", "odd"), chunk("zzzz", "?")});
    requireDirect(write("dup.sf2", dup.build()), {"A", "B"});
  }
  SECTION("rejected by libgig")
  {
    auto bad = sf;
    bad.infoList = list("INFO", {chunk("ifil", le16(3) + le16(0))});
    CHECK(requireParsed(write("version3.sf2", bad.build())).empty());

    bad = sf;
    bad.pdtaOverride["pbag"] = chunk("pbag", le16(0) + le16(0));
    CHECK(requireParsed(write("shortpbag.sf2", bad.build())).empty());

    bad = sf;
    bad.pdtaOverride["igen"] = chunk("igen", le16(53) + le16(0));
    CHECK(requireParsed(write("shortigen.sf2", bad.build())).empty());

    bad = sf;
    bad.pdtaOverride["imod"] = chunk("imod", zeros(7));
    CHECK(requireParsed(write("oddimod.sf2", bad.build())).empty());

    bad = sf;
    bad.pdtaOverride["shdr"] = "";
    CHECK(requireParsed(write("noshdr.sf2", bad.build())).empty());

    CHECK(requireParsed(write("notriff.sf2", std::string(512, 'x'))).empty());
    CHECK(requireParsed(write("zero.sf2", "")).empty());
    CHECK(listInstrumentNames("/nonexistent/dir/file.sf2").empty());
  }
  // libgig walks a list up to the size it declares, creating a chunk for
  // every four bytes missing from the file: sizes are kept small here
  SECTION("size fields beyond the file")
  {
    auto bad = sf;
    bad.pdtaOverride["phdr"] = chunkSized("phdr", 0xFFFFFFF0u, zeros(76));
    CHECK(requireParsed(write("hugephdr.sf2", bad.build())).empty());

    bad = sf;
    bad.infoList = list(
        "INFO", {chunk("ifil", le16(2) + le16(1)),
                 chunkSized("INAM", 0x00FFFFF0u, "name")});
    CHECK(sameAsLibgig(write("hugeinam.sf2", bad.build())));

    auto bytes = good;
    bytes.replace(4, 4, le32(2));
    CHECK(!readBankNames(write("tinyriff.sf2", bytes), BankKind::Sf2));
  }
  SECTION("big-endian files are not read here")
  {
    auto bytes = good;
    bytes.replace(0, 4, "RIFX");
    const char* why{};
    CHECK(!readBankNames(write("rifx.sf2", bytes), BankKind::Sf2, &why));
    CHECK(why);
  }
}

TEST_CASE("bank names: dls", "[deuterium][banknames]")
{
  const auto named = [](const std::string& name) {
    return dlsInstrument(0, 0, info(name));
  };

  SECTION("names, as stored")
  {
    requireDirect(
        write(
            "names.dls",
            dls({named("Grand Piano" + zeros(1)),
                 named("odd" + zeros(1)),         // odd-sized chunk, padded
                 named("NoTerminator"),           // fills its chunk
                 named("Cut" + zeros(1) + "tail" + zeros(1)),
                 named("  spaced  " + zeros(1)),
                 named("Fl\xC3\xBBte" + zeros(1)),
                 named("Cor fran\xE7" "ais" + zeros(2))})),
        {"Grand Piano", "odd", "NoTerminator", "Cut", "  spaced  ", "Fl\xC3\xBBte",
         "Cor fran\xE7" "ais"});
  }
  SECTION("placeholder names become the MIDI program")
  {
    requireDirect(
        write(
            "placeholders.dls",
            dls({dlsInstrument(0, 5, ""),                            // no INFO
                 dlsInstrument(0, 6, list("INFO", {chunk("ICMT", "x")})),
                 dlsInstrument(0x80000000u, 7, info("Drumset" + zeros(1))),
                 dlsInstrument(0x00000102u, 8, info("Instrument 3" + zeros(1))),
                 dlsInstrument(0x00007F7Fu, 9, info(zeros(4))),
                 list("ins ", {info("Kept" + zeros(1))}),            // no insh
                 list("ins ", {})})),
        {"Program 5", "Program 6", "Drum kit 7", "Bank 130, Program 8",
         "Bank 16383, Program 9", "Kept", "Program 0"});
  }
  SECTION("which chunk is the name")
  {
    requireDirect(
        write(
            "which.dls",
            dls({
                // the last INAM of the first INFO list
                dlsInstrument(
                    0, 0,
                    list("INFO", {chunk("INAM", "first"), chunk("INAM", "last")})
                        + info("second list")),
                // lists nested in the instrument do not name it
                dlsInstrument(0, 1, list("lart", {info("nested")})),
                // not an instrument
                list("ins2", {info("other")}),
                chunk("junk", "abc"),
            })),
        {"last", "Program 1"});
  }
  SECTION("no instrument")
  {
    requireDirect(write("empty.dls", dls({})), {});
    requireDirect(
        write(
            "nolins.dls", riff("DLS ", {chunk("colh", le32(0)), dlsWavePool()})),
        {});
    // only the first 'lins' list counts
    requireDirect(
        write(
            "twolins.dls",
            riff(
                "DLS ", {chunk("colh", le32(1)), list("lins", {named("one")}),
                         list("lins", {named("two")})})),
        {"one"});
  }
  SECTION("trailing bytes")
  {
    requireDirect(
        write("trailing.dls", dls({named("A" + zeros(1))}) + "junk"), {"A"});
    // a few stray bytes at the end of a list are not a chunk
    requireDirect(
        write(
            "stray.dls", riff(
                             "DLS ", {chunk("colh", le32(1)),
                                      list("lins", {named("A" + zeros(1)) + "abc"})
                                          + zeros(1)})),
        {"A"});
  }
  SECTION("conditional instruments are evaluated by libgig")
  {
    // "constant 0": the instrument is dropped
    const auto hidden = list(
        "ins ", {chunk("cdl ", le16(0x0010) + le32(0)), dlsInsh(0, 0),
                 info("Hidden" + zeros(1))});
    CHECK(
        requireParsed(write("cdl.dls", dls({hidden, named("Shown" + zeros(1))})))
        == Names{"Shown"});
  }
  SECTION("rejected by libgig")
  {
    CHECK(requireParsed(
              write("nocolh.dls", riff("DLS ", {list("lins", {named("A")})})))
              .empty());
    CHECK(requireParsed(
              write(
                  "shortinsh.dls",
                  dls({list("ins ", {chunk("insh", le16(1)), info("A")})})))
              .empty());
  }
  SECTION("size fields beyond the file")
  {
    CHECK(sameAsLibgig(write(
        "hugeinam.dls",
        dls({dlsInstrument(
            0, 0, list("INFO", {chunkSized("INAM", 0x00FFFFF0u, "name")}))}))));
    CHECK(sameAsLibgig(write(
        "longins.dls",
        riff(
            "DLS ", {chunk("colh", le32(1)),
                     list(
                         "lins",
                         {chunkSized("LIST", 400, "ins " + info("A" + zeros(1)))})}))));
    CHECK(sameAsLibgig(write(
        "hugeptbl.dls",
        riff(
            "DLS ", {chunk("colh", le32(1)), list("lins", {named("A" + zeros(1))}),
                     chunk("ptbl", le32(8) + le32(0x00FFFFFFu))}))));
  }
}

TEST_CASE("bank names: the extension picks the DLS parser", "[deuterium][banknames]")
{
  // A plain DLS named .gig goes through libgig's gig parser, which makes up
  // the gig-specific parts of every region
  const auto bytes = dls({dlsInstrument(0, 3, info("Organ" + zeros(1))),
                          dlsInstrument(0, 4, info("Instrument" + zeros(1)))});
  requireDirect(write("plain.dls", bytes), {"Organ", "Program 4"});
  CHECK(requireParsed(write("plain_dls.gig", bytes)) == Names{"Organ", "Instrument"});

  // A gig file named .dls is listed as a DLS
  const auto gig = read(makeGig(tmp("real.gig"), {"Violin", "Instrument 2", ""}));
  requireDirect(tmp("real.gig"), {"Violin", "Instrument 2", ""});
  requireDirect(write("gig_as.dls", gig), {"Violin", "Program 0", "Program 0"});

  // A SoundFont is recognized whatever its extension
  Sf2 sf;
  sf.presets = {"Preset"};
  requireDirect(write("soundfont.dls", sf.build()), {"Preset"});
  requireDirect(write("soundfont.gig", sf.build()), {"Preset"});

  // And a RIFF file of another kind is nobody's
  const auto wave = riff("WAVE", {chunk("fmt ", zeros(16)), chunk("data", zeros(8))});
  CHECK(requireParsed(write("wave.gig", wave)).empty());
  CHECK(requireParsed(write("wave.dls", wave)).empty());
  CHECK(requireParsed(write("wave.sf2", wave)).empty());
}

TEST_CASE("bank names: gig", "[deuterium][banknames]")
{
  const std::string full(64, 'n');
  requireDirect(
      makeGig(tmp("names.gig"), {"Violin", full, "", "Fl\xC3\xBBte", "  spaced "}),
      {"Violin", full, "", "Fl\xC3\xBBte", "  spaced "});
  requireDirect(makeGig(tmp("none.gig"), {}), {});

  const auto bytes = read(makeGig(tmp("base.gig"), {"One", "Two"}));
  requireDirect(write("trailing.gig", bytes + "garbage"), {"One", "Two"});

  // A region whose dimension regions are missing is rejected by libgig, and
  // with it the whole file
  auto broken = bytes;
  const auto prg = broken.find("3prg");
  REQUIRE(prg != std::string::npos);
  broken.replace(prg + 4, 4, "LIS_");
  CHECK(requireParsed(write("noewl.gig", broken)).empty());

  // A sample header libgig cannot read: same
  broken = bytes;
  const auto smpl = broken.find("smpl");
  REQUIRE(smpl != std::string::npos);
  broken.replace(smpl + 4, 4, le32(10));
  CHECK(sameAsLibgig(write("shortsmpl.gig", broken)));
}

TEST_CASE("bank names: gig velocity splits", "[deuterium][banknames]")
{
  const auto bytes = read(makeGig(tmp("velocity.gig"), {"Soft and loud"}, 63));
  requireDirect(tmp("velocity.gig"), {"Soft and loud"});

  // libgig lays the velocity zones out in a 128-entry table per region and
  // trusts the file for it: a region that does not fit is not for this
  // reader to answer for (and not for libgig to be shown here)
  const auto ewa = bytes.find("3ewa");
  REQUIRE(ewa != std::string::npos);
  REQUIRE(bytes.size() > ewa + 8 + 148);

  // a split point past the end of the table
  auto broken = bytes;
  broken[ewa + 8 + 140] = char(200);
  const char* why{};
  CHECK(!readBankNames(write("splitpoint.gig", broken), BankKind::Gig, &why));
  CHECK(why);

  // velocity zones for which the region has no dimension region
  const auto lnk = bytes.find("3lnk");
  REQUIRE(lnk != std::string::npos);
  broken = bytes;
  REQUIRE(uint8_t(broken[lnk + 8 + 4]) == 0x82); // the velocity dimension
  broken[lnk + 8 + 4 + 4] = char(200);           // its zones
  CHECK(!readBankNames(write("zones.gig", broken), BankKind::Gig, &why));

  // more dimension bits than a region can have
  broken = bytes;
  broken[lnk + 8 + 4 + 1] = char(31);
  CHECK(!readBankNames(write("bits.gig", broken), BankKind::Gig, &why));
}

TEST_CASE("bank names: truncated files", "[deuterium][banknames]")
{
  Sf2 sf;
  sf.presets = {"Piano", "Strings"};
  // A SoundFont's names are at its very end: no prefix is complete
  CHECK(truncations("truncated.sf2", sf.build()) == 0);

  truncations(
      "truncated.dls", dls({dlsInstrument(0, 1, info("Piano" + zeros(1))),
                            dlsInstrument(0, 2, info("Odd" + zeros(1)))}));

  truncations("truncated.gig", read(makeGig(tmp("whole.gig"), {"One", "Two"})));
}

TEST_CASE("bank names: damaged files", "[deuterium][banknames]")
{
  // The reader alone, on files libgig itself may not survive: whatever it
  // answers, it must neither read out of bounds nor run away
  Sf2 sf;
  sf.presets = {"Piano", "Strings"};
  const std::string bases[] = {
      sf.build(),
      dls({dlsInstrument(0, 1, info("Piano" + zeros(1))),
           dlsInstrument(0, 2, info("Odd" + zeros(1)))}),
      read(makeGig(tmp("damage.gig"), {"One", "Two"}))};

  std::mt19937 rng{20261118};
  int listed = 0;
  for(const auto& base : bases)
  {
    for(int i = 0; i < 1500; i++)
    {
      auto bytes = base;
      const int edits = 1 + int(rng() % 4);
      for(int e = 0; e < edits; e++)
      {
        const auto at = rng() % bytes.size();
        switch(rng() % 4)
        {
          case 0:
            bytes[at] = char(rng());
            break;
          case 1:
            bytes[at] = char(0xFF);
            break;
          case 2:
            bytes.replace(at, std::min<std::size_t>(4, bytes.size() - at), le32(rng()));
            break;
          default:
            bytes.resize(at);
            break;
        }
        if(bytes.empty())
          break;
      }
      const auto path = write("damaged.bin", bytes);
      for(const auto kind : {BankKind::Gig, BankKind::Dls, BankKind::Sf2})
      {
        const auto names = readBankNames(path, kind);
        if(names)
        {
          listed++;
          REQUIRE(names->size() < 1000);
        }
      }
    }
  }
  // Damage in the sample data leaves the names readable
  CHECK(listed > 0);
}

TEST_CASE("bank names: collection", "[deuterium][banknames]")
{
  const auto roots = qEnvironmentVariable("DEUTERIUM_BANK_ROOTS")
                         .split(QDir::listSeparator(), Qt::SkipEmptyParts);
  if(roots.isEmpty())
    SKIP("DEUTERIUM_BANK_ROOTS is not set");

  QStringList files;
  QSet<QString> seen;
  for(const auto& root : roots)
  {
    QDirIterator it{
        root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks};
    while(it.hasNext())
    {
      const QFileInfo file{it.next()};
      const auto suffix = file.suffix().toLower();
      if(suffix != "gig" && suffix != "dls" && suffix != "sf2")
        continue;
      // The same file reached through two links is one file
      const auto canonical = file.canonicalFilePath();
      if(canonical.isEmpty() || seen.contains(canonical))
        continue;
      seen.insert(canonical);
      files.push_back(file.absoluteFilePath());
    }
  }
  files.sort();

  struct Stats
  {
    int files{};
    int direct{};
    int parsed{};
    int empty{};
    int mismatches{};
    int entryMismatches{};
    std::size_t names{};
    double namesTime{};
    double libgigTime{};
    std::map<std::string, int> whyParsed;
  };
  std::map<std::string, Stats> stats;

  using clock = std::chrono::steady_clock;
  const auto seconds = [](clock::duration d) {
    return std::chrono::duration<double>(d).count();
  };

  LibraryHandler handler;
  Library::LibraryInterface& scanner = handler;
  const bool verbose = qEnvironmentVariableIsSet("DEUTERIUM_BANK_VERBOSE");
  // Parsing first puts the file in the page cache for the reader, the
  // other order does the reverse: time both orders for cold-cache numbers
  const bool libgigFirst = qEnvironmentVariableIsSet("DEUTERIUM_BANK_LIBGIG_FIRST");

  for(const auto& path : files)
  {
    if(verbose)
    {
      std::printf("%s\n", qPrintable(path));
      std::fflush(stdout);
    }
    auto& s = stats[formatName(path).toStdString()];
    s.files++;

    Names names, reference;
    InstrumentNamesSource source;
    const auto runNames = [&] {
      const auto t0 = clock::now();
      names = listInstrumentNames(path, &source);
      s.namesTime += seconds(clock::now() - t0);
    };
    const auto runLibgig = [&] {
      const auto t0 = clock::now();
      reference = listInstruments(path);
      s.libgigTime += seconds(clock::now() - t0);
    };
    if(libgigFirst)
    {
      runLibgig();
      runNames();
    }
    else
    {
      runNames();
      runLibgig();
    }

    s.names += reference.size();
    if(source.direct)
    {
      s.direct++;
    }
    else
    {
      s.parsed++;
      s.whyParsed[source.whyNotDirect ? source.whyNotDirect : "?"]++;
    }
    if(names.empty() && reference.empty())
      s.empty++;

    if(names != reference)
    {
      s.mismatches++;
      std::printf(
          "MISMATCH %s\n  names:  %s\n  libgig: %s\n", qPrintable(path),
          printable(names).c_str(), printable(reference).c_str());
    }

    // What the library shows for the file, from either list
    const auto utf8 = path.toStdString();
    const auto entry = scanner.scanPath(utf8);
    const auto referenceEntry
        = handler.scanPathWith(utf8, [&](const QString&) { return reference; });
    if(!sameEntry(entry, referenceEntry))
    {
      s.entryMismatches++;
      std::printf("ENTRY MISMATCH %s\n", qPrintable(path));
    }
  }

  int mismatches = 0;
  std::printf("\n%d files\n", int(files.size()));
  for(const auto& [format, s] : stats)
  {
    std::printf(
        "%s: %d files, %zu names, %d mismatches, %d library entry mismatches, "
        "%d read directly, %d parsed by libgig, %d without instrument in "
        "both\n  time: %.3f s (libgig alone: %.3f s)\n",
        format.c_str(), s.files, s.names, s.mismatches, s.entryMismatches,
        s.direct, s.parsed, s.empty, s.namesTime, s.libgigTime);
    for(const auto& [why, count] : s.whyParsed)
      std::printf("  parsed by libgig, %s: %d\n", why.c_str(), count);
    mismatches += s.mismatches + s.entryMismatches;
  }
  std::fflush(stdout);
  REQUIRE(mismatches == 0);
}
