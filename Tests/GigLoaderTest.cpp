// Unit tests for the Deuterium sample-file loader (GIG / DLS / SF2).
// Fixture files are generated on the fly: .gig and .dls through libgig's
// file-creation API, .sf2 as a hand-written minimal SoundFont.
#include <Deuterium/GigSampler/GigLoader.hpp>

#include <DLS.h>
#include <gig.h>

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>
#include <vector>

using namespace Deuterium::Gig;

namespace
{
static constexpr int FRAMES = 400;
static constexpr int RATE = 44100;

std::vector<int16_t> rampData()
{
  std::vector<int16_t> data(FRAMES);
  for(int i = 0; i < FRAMES; i++)
    data[i] = int16_t(i * 16);
  return data;
}

// Creates a mono 16-bit gig file with `instruments` instruments of one
// region each (named TestInstrument, TestInstrument2, ...).
// velocityLimits: if non-empty, adds a 2-zone velocity dimension with the
//                 given per-zone upper limits to the first instrument.
// loopEnd: if > 0, defines a forward loop [loopStart, loopEnd] on the sample.
QString makeGigFile(
    const QString& path, std::vector<int> velocityLimits = {},
    uint32_t loopStart = 0, uint32_t loopEnd = 0, int instruments = 1,
    uint32_t sampleRate = RATE)
{
  gig::File f;
  f.pInfo->Name = "TestBank";

  auto data = rampData();

  gig::Sample* smp = f.AddSample();
  smp->pInfo->Name = "TestSample";
  smp->Channels = 1;
  smp->BitDepth = 16;
  smp->FrameSize = 2;
  smp->SamplesPerSecond = sampleRate;
  smp->MIDIUnityNote = 60;
  if(loopEnd > 0)
  {
    smp->Loops = 1;
    smp->LoopType = gig::loop_type_normal;
    smp->LoopStart = loopStart;
    smp->LoopEnd = loopEnd;
    smp->LoopSize = loopEnd - loopStart;
  }
  smp->Resize(FRAMES);

  for(int i = 0; i < instruments; i++)
  {
    gig::Instrument* ins = f.AddInstrument();
    ins->pInfo->Name
        = i == 0 ? std::string("TestInstrument")
                 : std::string("TestInstrument") + std::to_string(i + 1);

    gig::Region* rgn = ins->AddRegion();
    rgn->SetKeyRange(36 + i, 48 + i);
    rgn->SetSample(smp);
    rgn->pDimensionRegions[0]->pSample = smp;

    if(i == 0 && !velocityLimits.empty())
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
        if(z < velocityLimits.size())
        {
          dr->DimensionUpperLimits[0] = velocityLimits[z];
          dr->VelocityUpperLimit = velocityLimits[z];
        }
      }
    }
  }

  f.Save(path.toStdString());

  // Sample data can only be written once the chunks exist on disk
  smp->SetPos(0);
  smp->Write(data.data(), FRAMES);

  return path;
}

// Creates a mono 16-bit DLS file with one instrument and one region.
QString makeDlsFile(const QString& path)
{
  DLS::File f;
  f.pInfo->Name = "TestDlsBank";

  auto data = rampData();

  DLS::Sample* smp = f.AddSample();
  smp->pInfo->Name = "TestSample";
  smp->FormatTag = DLS_WAVE_FORMAT_PCM;
  smp->Channels = 1;
  smp->BitDepth = 16;
  smp->BlockAlign = 2;
  smp->FrameSize = 2;
  smp->SamplesPerSecond = RATE;
  smp->AverageBytesPerSecond = RATE * 2;
  smp->Resize(FRAMES);

  DLS::Instrument* ins = f.AddInstrument();
  ins->pInfo->Name = "TestDlsInstrument";

  DLS::Region* rgn = ins->AddRegion();
  rgn->SetKeyRange(40, 52);
  rgn->VelocityRange = {20, 100};
  rgn->UnityNote = 62;
  rgn->SetSample(smp);

  DLS::sample_loop_t loop{};
  loop.Size = sizeof(DLS::sample_loop_t);
  loop.LoopType = 0;
  loop.LoopStart = 10;
  loop.LoopLength = 50;
  rgn->AddSampleLoop(&loop);

  f.Save(path.toStdString());

  smp->SetPos(0);
  smp->Write(data.data(), FRAMES);

  return path;
}

// Minimal SoundFont 2 writer: one preset -> one instrument -> one mono
// 16-bit sample; libgig has no sf2 creation API so the chunks are written
// by hand.
namespace sf2writer
{
void u8(QByteArray& b, uint8_t v)
{
  b.append(char(v));
}
void u16(QByteArray& b, uint16_t v)
{
  b.append(char(v & 0xFF));
  b.append(char(v >> 8));
}
void u32(QByteArray& b, uint32_t v)
{
  u16(b, v & 0xFFFF);
  u16(b, v >> 16);
}
void fourcc(QByteArray& b, const char* f)
{
  b.append(f, 4);
}
void name20(QByteArray& b, const char* n)
{
  char buf[20]{};
  qstrncpy(buf, n, sizeof(buf));
  b.append(buf, 20);
}
QByteArray chunk(const char* id, const QByteArray& payload)
{
  QByteArray b;
  fourcc(b, id);
  u32(b, payload.size());
  b.append(payload);
  if(payload.size() % 2)
    u8(b, 0);
  return b;
}
QByteArray list(const char* type, const QByteArray& contents)
{
  QByteArray payload;
  fourcc(payload, type);
  payload.append(contents);
  return chunk("LIST", payload);
}

// SF2 generator opcodes
enum
{
  gen_instrument = 41,
  gen_keyRange = 43,
  gen_velRange = 44,
  gen_sampleID = 53,
  gen_sampleModes = 54
};

void gen(QByteArray& b, uint16_t op, uint16_t amount)
{
  u16(b, op);
  u16(b, amount);
}
void genRange(QByteArray& b, uint16_t op, uint8_t lo, uint8_t hi)
{
  u16(b, op);
  u8(b, lo);
  u8(b, hi);
}
}

// Writes a mono 16-bit PCM WAV with the ramp data
void writeWavFile(const QString& path)
{
  using namespace sf2writer;
  auto data = rampData();

  QByteArray fmt;
  u16(fmt, 1); // PCM
  u16(fmt, 1); // mono
  u32(fmt, RATE);
  u32(fmt, RATE * 2);
  u16(fmt, 2);  // block align
  u16(fmt, 16); // bits

  QByteArray smp;
  for(int16_t s : data)
    u16(smp, uint16_t(s));

  QByteArray contents = chunk("fmt ", fmt) + chunk("data", smp);
  QByteArray file;
  fourcc(file, "RIFF");
  u32(file, contents.size() + 4);
  fourcc(file, "WAVE");
  file.append(contents);

  QFile out(path);
  out.open(QIODevice::WriteOnly);
  out.write(file);
  out.close();
}

// Creates a Hydrogen drumkit folder: drumkit.xml + wav layers.
// One modern-format instrument with two velocity layers, one oldest-format
// instrument with a bare filename.
QString makeHydrogenKit(const QString& dirPath)
{
  QDir{}.mkpath(dirPath);
  writeWavFile(dirPath + "/kick_soft.wav");
  writeWavFile(dirPath + "/kick_hard.wav");
  writeWavFile(dirPath + "/snare.wav");

  const char* xml = R"_(<?xml version="1.0" encoding="UTF-8"?>
<drumkit_info>
  <name>TestKit</name>
  <author>tests</author>
  <info></info>
  <license>CC0</license>
  <instrumentList>
    <instrument>
      <id>0</id>
      <name>Kick</name>
      <midiOutNote>36</midiOutNote>
      <volume>0.8</volume>
      <isMuted>false</isMuted>
      <pan_L>1.0</pan_L>
      <pan_R>1.0</pan_R>
      <randomPitchFactor>0.1</randomPitchFactor>
      <applyVelocity>true</applyVelocity>
      <muteGroup>1</muteGroup>
      <sampleSelectionAlgo>ROUND_ROBIN</sampleSelectionAlgo>
      <filterActive>true</filterActive>
      <filterCutoff>0.5</filterCutoff>
      <filterResonance>0.2</filterResonance>
      <Attack>0</Attack>
      <Decay>44100</Decay>
      <Sustain>1</Sustain>
      <Release>1000</Release>
      <instrumentComponent>
        <component_id>0</component_id>
        <layer>
          <filename>kick_soft.wav</filename>
          <min>0.0</min>
          <max>0.5</max>
          <gain>1.0</gain>
          <pitch>0.0</pitch>
        </layer>
        <layer>
          <filename>kick_hard.wav</filename>
          <min>0.5</min>
          <max>1.0</max>
          <gain>0.5</gain>
          <pitch>-1.0</pitch>
        </layer>
      </instrumentComponent>
    </instrument>
    <instrument>
      <id>1</id>
      <name>Snare</name>
      <midiOutNote>38</midiOutNote>
      <volume>1.0</volume>
      <isMuted>true</isMuted>
      <pan_L>1.0</pan_L>
      <pan_R>0.5</pan_R>
      <filename>snare.wav</filename>
    </instrument>
  </instrumentList>
</drumkit_info>
)_";

  QFile out(dirPath + "/drumkit.xml");
  out.open(QIODevice::WriteOnly);
  out.write(xml);
  out.close();
  return dirPath + "/drumkit.xml";
}

QString makeSf2File(const QString& path)
{
  using namespace sf2writer;
  auto data = rampData();

  // INFO
  QByteArray ifil;
  u16(ifil, 2);
  u16(ifil, 1);
  QByteArray infoList = chunk("ifil", ifil) + chunk("isng", QByteArray("EMU8000", 8))
                        + chunk("INAM", QByteArray("TestSf2Bank", 12));

  // sdta: samples + mandatory >= 46 zero guard points
  QByteArray smpl;
  for(int16_t s : data)
    u16(smpl, uint16_t(s));
  for(int i = 0; i < 46; i++)
    u16(smpl, 0);
  QByteArray sdtaList = chunk("smpl", smpl);

  // pdta
  QByteArray phdr;
  name20(phdr, "TestPreset");
  u16(phdr, 0); // preset number
  u16(phdr, 0); // bank
  u16(phdr, 0); // preset bag index
  u32(phdr, 0);
  u32(phdr, 0);
  u32(phdr, 0);
  name20(phdr, "EOP");
  u16(phdr, 0);
  u16(phdr, 0);
  u16(phdr, 1); // terminal bag index
  u32(phdr, 0);
  u32(phdr, 0);
  u32(phdr, 0);

  QByteArray pbag;
  u16(pbag, 0);
  u16(pbag, 0); // zone 0 -> pgen index 0
  u16(pbag, 2);
  u16(pbag, 0); // terminal

  QByteArray pmod(10, '\0');

  QByteArray pgen;
  genRange(pgen, gen_keyRange, 0, 127);
  gen(pgen, gen_instrument, 0);
  gen(pgen, 0, 0); // terminal

  QByteArray inst;
  name20(inst, "TestInst");
  u16(inst, 0);
  name20(inst, "EOI");
  u16(inst, 1);

  QByteArray ibag;
  u16(ibag, 0);
  u16(ibag, 0);
  u16(ibag, 4);
  u16(ibag, 0); // terminal

  QByteArray imod(10, '\0');

  QByteArray igen;
  genRange(igen, gen_keyRange, 30, 90);
  genRange(igen, gen_velRange, 10, 100);
  gen(igen, gen_sampleModes, 1); // looping enabled
  gen(igen, gen_sampleID, 0);
  gen(igen, 0, 0); // terminal

  QByteArray shdr;
  name20(shdr, "TestSample");
  u32(shdr, 0);            // start
  u32(shdr, FRAMES);       // end
  u32(shdr, 10);           // start loop
  u32(shdr, 90);           // end loop
  u32(shdr, RATE);         // sample rate
  u8(shdr, 64);            // original pitch
  u8(shdr, 0);             // pitch correction
  u16(shdr, 0);            // sample link
  u16(shdr, 1);            // mono sample
  name20(shdr, "EOS");
  u32(shdr, 0);
  u32(shdr, 0);
  u32(shdr, 0);
  u32(shdr, 0);
  u32(shdr, 0);
  u8(shdr, 0);
  u8(shdr, 0);
  u16(shdr, 0);
  u16(shdr, 0);

  QByteArray pdtaList = chunk("phdr", phdr) + chunk("pbag", pbag) + chunk("pmod", pmod)
                        + chunk("pgen", pgen) + chunk("inst", inst) + chunk("ibag", ibag)
                        + chunk("imod", imod) + chunk("igen", igen) + chunk("shdr", shdr);

  QByteArray contents
      = list("INFO", infoList) + list("sdta", sdtaList) + list("pdta", pdtaList);

  QByteArray file;
  fourcc(file, "RIFF");
  u32(file, contents.size() + 4);
  fourcc(file, "sfbk");
  file.append(contents);

  QFile out(path);
  out.open(QIODevice::WriteOnly);
  out.write(file);
  out.close();
  return path;
}

}

class GigLoaderTest final : public QObject
{
  Q_OBJECT
  QTemporaryDir m_dir;

  QString tmp(const QString& name) { return m_dir.filePath(name); }

private Q_SLOTS:

  void test_missing_files_return_null()
  {
    QVERIFY(!loadGigFileMetadata("/nonexistent/dir/foo.gig"));
    QVERIFY(!loadGigFileMetadata("/nonexistent/dir/foo.dls"));
    QVERIFY(!loadGigFileMetadata("/nonexistent/dir/foo.sf2"));
    QVERIFY(!loadGigFileSamples(nullptr, RATE, {}));
  }

  void test_garbage_file_returns_null()
  {
    const auto path = tmp("garbage.sf2");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray(512, 'x'));
    f.close();
    QVERIFY(!loadGigFileMetadata(path));

    // A valid file with the wrong extension is recognized by content
    // sniffing (sample banks in the wild frequently carry wrong extensions)
    const auto gigAsSf2 = makeGigFile(tmp("actually_a.gig"));
    QFile::copy(gigAsSf2, tmp("wrongform.sf2"));
    auto sniffed = loadGigFileMetadata(tmp("wrongform.sf2"));
    QVERIFY(sniffed);
    QCOMPARE(sniffed->instruments.size(), std::size_t(1));
  }

  void test_gig_metadata()
  {
    const auto path = makeGigFile(tmp("basic.gig"));
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    QCOMPARE(info->name, std::string("TestBank"));
    QCOMPARE(info->instruments.size(), std::size_t(1));
    QCOMPARE(info->instruments[0].name, std::string("TestInstrument"));

    auto& regions = info->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(1));
    QCOMPARE(int(regions[0].keyLow), 36);
    QCOMPARE(int(regions[0].keyHigh), 48);
    QCOMPARE(int(regions[0].velLow), 0);
    QCOMPARE(int(regions[0].velHigh), 127);
    QCOMPARE(regions[0].sample.midiUnityNote, uint32_t(60));
    QVERIFY(regions[0].sample.data.empty()); // metadata only
  }

  void test_gig_samples()
  {
    const auto path = makeGigFile(tmp("samples.gig"));
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);

    auto& regions = loaded->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(1));
    auto& sample = regions[0].sample;
    QCOMPARE(sample.data.size(), std::size_t(1));
    QCOMPARE(sample.data[0].size(), std::size_t(FRAMES));

    // Values must match the known ramp within 16 bit quantization
    auto ramp = rampData();
    for(int i : {0, 1, 100, FRAMES - 1})
      QVERIFY(std::abs(sample.data[0][i] - ramp[i] / 32768.0) < 1e-9);
  }

  void test_gig_velocity_zones_non_uniform()
  {
    const auto path = makeGigFile(tmp("velsplit.gig"), {40, 127});
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);

    auto& regions = info->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(2));

    // Zones must be contiguous and non-overlapping: 0-40, 41-127
    QCOMPARE(int(regions[0].velLow), 0);
    QCOMPARE(int(regions[0].velHigh), 40);
    QCOMPARE(int(regions[1].velLow), 41);
    QCOMPARE(int(regions[1].velHigh), 127);
  }

  void test_gig_loop_points_clamped_to_sample_length()
  {
    // Loop end far beyond the actual sample data
    const auto path = makeGigFile(tmp("loop.gig"), {}, 100, 100000);
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);

    auto& sample = loaded->instruments[0].regions[0].sample;
    const auto frames = sample.data[0].size();
    if(sample.hasLoop)
    {
      QVERIFY(sample.loopEnd <= frames);
      QVERIFY(sample.loopStart < sample.loopEnd);
    }
  }

  void test_gig_resampling_keeps_loop_valid()
  {
    const auto path = makeGigFile(tmp("resample.gig"), {}, 100, 100000);
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    auto loaded = loadGigFileSamples(info, 96000, {});
    QVERIFY(loaded);

    auto& sample = loaded->instruments[0].regions[0].sample;
    QCOMPARE(sample.sampleRate, uint32_t(96000));
    const auto frames = sample.data[0].size();
    QVERIFY(frames > std::size_t(FRAMES)); // upsampled
    if(sample.hasLoop)
      QVERIFY(sample.loopEnd <= frames);
  }

  void test_truncated_gig_does_not_crash()
  {
    const auto path = makeGigFile(tmp("trunc.gig"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    auto bytes = f.readAll();
    f.close();

    // Chop the file in the middle of the sample data
    const auto truncated = tmp("truncated.gig");
    QFile out(truncated);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(bytes.constData(), bytes.size() * 6 / 10);
    out.close();

    // Whatever the outcome (null or pruned regions), it must not crash and
    // any surviving region's loop points must stay inside the decoded data.
    auto info = loadGigFileMetadata(truncated);
    if(info)
    {
      auto loaded = loadGigFileSamples(info, RATE, {});
      if(loaded)
      {
        for(auto& r : loaded->instruments[loaded->selectedInstrument].regions)
        {
          QVERIFY(!r.sample.data.empty());
          if(r.sample.hasLoop)
            QVERIFY(r.sample.loopEnd <= r.sample.data[0].size());
        }
      }
    }
  }

  void test_parse_instrument_path()
  {
    auto plain = parseInstrumentPath("/some/file.gig");
    QCOMPARE(plain.file, QStringLiteral("/some/file.gig"));
    QCOMPARE(plain.instrument, 0);

    auto indexed = parseInstrumentPath("/some/file.sf2|3");
    QCOMPARE(indexed.file, QStringLiteral("/some/file.sf2"));
    QCOMPARE(indexed.instrument, 3);

    // Non-numeric suffix after '|' stays part of the path
    auto weird = parseInstrumentPath("/some/we|ird.gig");
    QCOMPARE(weird.file, QStringLiteral("/some/we|ird.gig"));
    QCOMPARE(weird.instrument, 0);

    // An existing file whose name contains "|<number>" is not split
    const auto trap = tmp("trap|2");
    QFile f(trap);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();
    auto existing = parseInstrumentPath(trap);
    QCOMPARE(existing.file, trap);
    QCOMPARE(existing.instrument, 0);
  }

  void test_multi_instrument_gig()
  {
    const auto path
        = makeGigFile(tmp("multi.gig"), {}, 0, 0, /* instruments: */ 3);

    const auto names = listInstruments(path);
    QCOMPARE(names.size(), std::size_t(3));
    QCOMPARE(names[0], std::string("TestInstrument"));
    QCOMPARE(names[1], std::string("TestInstrument2"));
    QCOMPARE(names[2], std::string("TestInstrument3"));

    // Loading with an explicit instrument index selects that instrument
    auto info = loadGigFileMetadata(path, 1);
    QVERIFY(info);
    QCOMPARE(info->selectedInstrument, 1);
    auto& regions = info->instruments[1].regions;
    QCOMPARE(regions.size(), std::size_t(1));
    QCOMPARE(int(regions[0].keyLow), 37);
    QCOMPARE(int(regions[0].keyHigh), 49);

    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);
    QCOMPARE(loaded->instruments[1].regions.size(), std::size_t(1));
    QVERIFY(!loaded->instruments[1].regions[0].sample.data.empty());

    // Out-of-range index falls back to instrument 0
    auto fallback = loadGigFileMetadata(path, 42);
    QVERIFY(fallback);
    QCOMPARE(fallback->selectedInstrument, 0);

    QCOMPARE(listInstruments("/nonexistent/file.gig").size(), std::size_t(0));
  }

  void test_dls()
  {
    const auto path = makeDlsFile(tmp("basic.dls"));
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    QCOMPARE(info->name, std::string("TestDlsBank"));
    QCOMPARE(info->instruments.size(), std::size_t(1));
    QCOMPARE(info->instruments[0].name, std::string("TestDlsInstrument"));

    auto& regions = info->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(1));
    QCOMPARE(int(regions[0].keyLow), 40);
    QCOMPARE(int(regions[0].keyHigh), 52);
    QCOMPARE(int(regions[0].velLow), 20);
    QCOMPARE(int(regions[0].velHigh), 100);
    QCOMPARE(regions[0].sample.midiUnityNote, uint32_t(62));
    QVERIFY(regions[0].sample.hasLoop);
    QCOMPARE(regions[0].sample.loopStart, uint32_t(10));
    QCOMPARE(regions[0].sample.loopEnd, uint32_t(60));

    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);
    auto& sample = loaded->instruments[0].regions[0].sample;
    QCOMPARE(sample.data.size(), std::size_t(1));
    QCOMPARE(sample.data[0].size(), std::size_t(FRAMES));

    auto ramp = rampData();
    for(int i : {0, 50, FRAMES - 1})
      QVERIFY(std::abs(sample.data[0][i] - ramp[i] / 32768.0) < 1e-9);
  }

  // A header claiming a zero (or absurd) sample rate must neither divide by
  // zero nor allocate a giant resample buffer
  void test_zero_sample_rate_is_survivable()
  {
    const auto path = makeGigFile(tmp("zerorate.gig"), {}, 0, 0, 1, 0);
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);
    auto& regions = loaded->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(1));
    QCOMPARE(regions[0].sample.data[0].size(), std::size_t(FRAMES));
    QCOMPARE(regions[0].sample.sampleRate, uint32_t(RATE));
    for(auto s : regions[0].sample.data[0])
      QVERIFY(std::isfinite(s));
  }

  // A gig round-robin dimension produces alternation indices instead of
  // stacked duplicate regions
  void test_gig_round_robin_dimension()
  {
    const auto path = tmp("rr.gig");
    {
      gig::File f;
      auto data = rampData();
      gig::Sample* smp = f.AddSample();
      smp->Channels = 1;
      smp->BitDepth = 16;
      smp->FrameSize = 2;
      smp->SamplesPerSecond = RATE;
      smp->MIDIUnityNote = 60;
      smp->Resize(FRAMES);

      gig::Instrument* ins = f.AddInstrument();
      ins->pInfo->Name = "RRInstr";
      gig::Region* rgn = ins->AddRegion();
      rgn->SetKeyRange(40, 50);
      rgn->SetSample(smp);
      rgn->pDimensionRegions[0]->pSample = smp;

      gig::dimension_def_t def{};
      def.dimension = gig::dimension_roundrobin;
      def.bits = 1;
      def.zones = 2;
      rgn->AddDimension(&def);
      for(uint32_t z = 0; z < rgn->DimensionRegions; z++)
        rgn->pDimensionRegions[z]->pSample = smp;

      f.Save(path.toStdString());
      smp->SetPos(0);
      smp->Write(data.data(), FRAMES);
    }

    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    auto& regions = info->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(2));
    QCOMPARE(regions[0].rrIndex, 0);
    QCOMPARE(regions[1].rrIndex, 1);
    QCOMPARE(regions[0].selectionAlgo, 1);
    // Same zone: identical key/velocity ranges
    QCOMPARE(regions[0].keyLow, regions[1].keyLow);
    QCOMPARE(regions[0].velLow, regions[1].velLow);
    QCOMPARE(regions[0].velHigh, regions[1].velHigh);
  }

  // libgig LoopEnd is an inclusive last-sample index; the engine's loop.end
  // is exclusive, so the loader converts
  void test_gig_loop_end_is_converted_to_exclusive()
  {
    const auto path = makeGigFile(tmp("loopend.gig"), {}, 100, 200);
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    auto& sample = info->instruments[0].regions[0].sample;
    QVERIFY(sample.hasLoop);
    QCOMPARE(sample.loopStart, uint32_t(100));
    QCOMPARE(sample.loopEnd, uint32_t(201));
  }

  void test_hydrogen_drumkit()
  {
    const auto path = makeHydrogenKit(tmp("mykit"));
    QCOMPARE(formatName(path), QStringLiteral("Drumkit"));

    const auto names = listInstruments(path);
    QCOMPARE(names.size(), std::size_t(1));
    QCOMPARE(names[0], std::string("TestKit"));

    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    QCOMPARE(info->name, std::string("TestKit"));
    QCOMPARE(info->instruments.size(), std::size_t(1));

    auto& regions = info->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(3));

    // Kick, layer 1: velocity range is half-open, so 0-0.5 -> 0..63 and the
    // boundary velocity belongs to the upper layer only
    QCOMPARE(int(regions[0].keyLow), 36);
    QCOMPARE(int(regions[0].keyHigh), 36);
    QVERIFY(regions[0].oneShot);
    QVERIFY(!regions[0].pitchTrack);
    QVERIFY(!regions[0].muted);
    QVERIFY(regions[0].applyVelocity);
    QCOMPARE(int(regions[0].velLow), 0);
    QCOMPARE(int(regions[0].velHigh), 63);
    QVERIFY(regions[0].velHigh < regions[1].velLow); // no double-trigger
    QVERIFY(regions[0].vcfEnabled);
    QCOMPARE(regions[0].randomPitch, 0.1);
    QCOMPARE(regions[0].chokeGroup, 1);
    QCOMPARE(regions[2].chokeGroup, -1);
    QCOMPARE(regions[0].selectionAlgo, 1); // ROUND_ROBIN
    QCOMPARE(regions[2].selectionAlgo, 0);
    // ADSR: Hydrogen frames at 44.1kHz -> seconds
    QCOMPARE(regions[0].eg1Decay, 1.0);
    QVERIFY(std::abs(regions[0].eg1Release - 1000.0 / 44100.0) < 1e-9);
    QVERIFY(std::abs(regions[0].sampleAttenuation - 0.8) < 1e-9);

    // Kick, layer 2: gain and pitch offset applied
    QCOMPARE(int(regions[1].velLow), 64);
    QCOMPARE(int(regions[1].velHigh), 127);
    QVERIFY(std::abs(regions[1].sampleAttenuation - 0.4) < 1e-9);
    QCOMPARE(regions[1].pitchOffset, -1.0);

    // Snare: oldest format, muted, panned left
    QCOMPARE(int(regions[2].keyLow), 38);
    QVERIFY(regions[2].muted);
    QVERIFY(regions[2].pan < 0);
    QCOMPARE(int(regions[2].velLow), 0);
    QCOMPARE(int(regions[2].velHigh), 127);

    // Phase 2: decode the wav layers
    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);
    auto& lregions = loaded->instruments[0].regions;
    QCOMPARE(lregions.size(), std::size_t(3));
    for(auto& r : lregions)
    {
      QVERIFY(!r.sample.data.empty());
      QCOMPARE(r.sample.data[0].size(), std::size_t(FRAMES));
    }
  }

  // Hand-edited kits in the wild contain nan/inf/out-of-range values; none
  // of that may ever reach the audio thread.
  void test_hostile_drumkit_is_sanitized()
  {
    const auto dirPath = tmp("evilkit");
    QDir{}.mkpath(dirPath);
    writeWavFile(dirPath + "/hit.wav");

    const char* xml = R"_(<?xml version="1.0" encoding="UTF-8"?>
<drumkit_info>
  <name>EvilKit</name>
  <instrumentList>
    <instrument>
      <id>0</id>
      <name>Evil</name>
      <midiOutNote>40</midiOutNote>
      <volume>nan</volume>
      <randomPitchFactor>inf</randomPitchFactor>
      <muteGroup>3</muteGroup>
      <pan>5</pan>
      <Attack>-500000</Attack>
      <Decay>nan</Decay>
      <Sustain>7</Sustain>
      <Release>1e20</Release>
      <instrumentComponent>
        <layer>
          <filename>hit.wav</filename>
          <min>0.9</min>
          <max>0.1</max>
          <gain>1e9</gain>
          <pitch>nan</pitch>
        </layer>
      </instrumentComponent>
    </instrument>
  </instrumentList>
</drumkit_info>
)_";
    QFile out(dirPath + "/drumkit.xml");
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(xml);
    out.close();

    auto info = loadGigFileMetadata(dirPath + "/drumkit.xml");
    QVERIFY(info);
    auto& regions = info->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(1));
    auto& r = regions[0];

    // Everything finite and in range
    QVERIFY(std::isfinite(r.eg1Attack) && r.eg1Attack >= 0. && r.eg1Attack <= 60.);
    QVERIFY(std::isfinite(r.eg1Decay) && r.eg1Decay >= 0. && r.eg1Decay <= 60.);
    QVERIFY(std::isfinite(r.eg1Release) && r.eg1Release >= 0. && r.eg1Release <= 60.);
    QVERIFY(r.eg1Sustain >= 0. && r.eg1Sustain <= 1.);
    QVERIFY(std::isfinite(r.sampleAttenuation) && r.sampleAttenuation <= 8.);
    QVERIFY(std::isfinite(r.pitchOffset));
    QCOMPARE(r.pitchOffset, 0.); // nan -> default
    QCOMPARE(r.randomPitch, 0.); // inf -> default
    QCOMPARE(int(r.pan), 63);    // 5 -> clamped hard right
    QCOMPARE(r.chokeGroup, 3);

    // Swapped velocity bounds are reordered
    QVERIFY(r.velLow <= r.velHigh);

    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);
    for(auto& lr : loaded->instruments[0].regions)
      for(auto& ch : lr.sample.data)
        for(auto s : ch)
          QVERIFY(std::isfinite(s));
  }

  void test_sf2()
  {
    const auto path = makeSf2File(tmp("basic.sf2"));
    auto info = loadGigFileMetadata(path);
    QVERIFY(info);
    QCOMPARE(info->instruments.size(), std::size_t(1));
    QCOMPARE(info->instruments[0].name, std::string("TestPreset"));

    auto& regions = info->instruments[0].regions;
    QCOMPARE(regions.size(), std::size_t(1));
    QCOMPARE(int(regions[0].keyLow), 30);
    QCOMPARE(int(regions[0].keyHigh), 90);
    QCOMPARE(int(regions[0].velLow), 10);
    QCOMPARE(int(regions[0].velHigh), 100);
    QCOMPARE(regions[0].sample.midiUnityNote, uint32_t(64));
    QVERIFY(regions[0].sample.hasLoop);
    QCOMPARE(regions[0].sample.loopStart, uint32_t(10));
    QCOMPARE(regions[0].sample.loopEnd, uint32_t(90));

    auto loaded = loadGigFileSamples(info, RATE, {});
    QVERIFY(loaded);
    QCOMPARE(loaded->instruments[0].regions.size(), std::size_t(1));
    auto& sample = loaded->instruments[0].regions[0].sample;
    QVERIFY(!sample.data.empty());
    QCOMPARE(sample.data[0].size(), std::size_t(FRAMES));

    auto ramp = rampData();
    for(int i : {0, 50, FRAMES - 1})
      QVERIFY(std::abs(sample.data[0][i] - ramp[i] / 32768.0) < 1e-9);
  }
};

QTEST_APPLESS_MAIN(GigLoaderTest)
#include "GigLoaderTest.moc"
