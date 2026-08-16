// Crash-hunting scanner for sample libraries (GIG / DLS / SF2).
//
// Loads every sample file through the exact same code path score uses
// (loadGigFileMetadata + loadGigFileSamples for every instrument of the
// file), each file in an isolated child process so that one crashing file
// cannot take down the scan.
//
//   Deuterium_PackTester [options] [paths...]
//     paths            files or directories to scan (default:
//                      ~/Documents/ossia/score/packages/samples)
//     --metadata-only  skip sample data decoding (fast parser check)
//     --single <file>  internal: test one file in-process
//     -j <N>           concurrent child processes (default 4)
//     --timeout <sec>  per-file timeout (default 600)
#include <Deuterium/GigSampler/GigLoader.hpp>
#include <Deuterium/GigSampler/SamplerEngine.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QProcess>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
enum ExitCode
{
  Ok = 0,
  Unloadable = 2,   // metadata parse returned null
  LoadFailed = 3,   // metadata ok but sample loading returned null
  EmptyLoad = 4,    // loaded "successfully" but no region has sample data
  BadPlayback = 5   // rendering produced NaN/inf, out-of-range access or a
                    // voice that never terminates
};

// Renders one region through the same primitives the executor uses (pitch,
// envelope, loop resolution, play head, sample read) and validates that the
// output is finite and that non-looping playback terminates. Catches broken
// loop points, out-of-range offsets and denormal/NaN sample data across the
// whole library, without needing the execution graph.
bool playbackRegionOk(const Deuterium::Gig::GigRegion& r)
{
  using namespace Deuterium::Gig;
  if(!r.sample.data || r.sample.data->empty() || (*r.sample.data)[0].empty())
    return true; // vetted separately (EmptyLoad handles the all-empty case)

  const auto& data = *r.sample.data;
  const int64_t frames = std::ssize(data[0]);
  const int channels = (int)data.size();
  constexpr double rate = 48000.;

  SamplerParams p; // defaults = "everything from file"

  const int notes[3]
      = {r.keyLow, std::clamp((r.keyLow + r.keyHigh) / 2, 0, 127), r.keyHigh};
  for(const int note : notes)
  {
    const double ratio
        = semitonesToRatio(basePitchSemitones(r, p, note));
    if(!std::isfinite(ratio) || ratio <= 0.)
      return false;

    auto loop = resolveLoop(r, p, frames);

    AmpEnv env;
    env.set_sample_rate(rate);
    env.attack(std::max(0.001, r.eg1Attack));
    env.hold(r.eg1Hold);
    env.decay(std::max(0.001, r.eg1Decay));
    env.sustain(r.eg1Sustain);
    env.release(std::max(0.016, r.eg1Release));
    env.dbMode(r.eg1DbSlope);
    env.reset();

    PlayHead head;
    head.pos = std::min<int64_t>(r.sampleStartOffset, frames - 1);
    head.dir = 1;

    int64_t effFrames = frames;
    if(r.sampleEndOffset > 0)
      effFrames = std::max<int64_t>(
          {(int64_t)1, frames - (int64_t)r.sampleEndOffset, (int64_t)loop.end});

    // Render up to 2 seconds; send the note-off after 0.5 s. An iteration
    // cap converts an infinite loop into a detectable failure.
    const int64_t maxIters = (int64_t)(2.0 * rate);
    const int64_t offAt = (int64_t)(0.5 * rate);
    bool released = false;
    bool ended = false;
    for(int64_t k = 0; k < maxIters; k++)
    {
      if(k == offAt)
      {
        env.startRelease();
        released = true;
      }
      const auto sr = readPosition(head.pos, effFrames, loop, 0, released);
      if(sr.idx < 0 || sr.idx >= frames)
        return false; // out-of-range read
      for(int c = 0; c < channels; c++)
      {
        const double v = data[c][sr.idx] * env();
        if(!std::isfinite(v))
          return false;
      }
      if(!head.advance(ratio, loop, effFrames, released))
      {
        ended = true;
        break;
      }
      if(env.done())
      {
        ended = true;
        break;
      }
    }
    // After release, every voice must terminate: a voice still running two
    // seconds after a 16ms..8s release window is the stuck-note bug class.
    // Looping regions with long releases legitimately outlive the cap only
    // if the release is close to it; allow those.
    if(!ended)
    {
      const double relSecs = std::max(0.016, r.eg1Release);
      if(relSecs < 1.2)
        return false;
    }
  }
  return true;
}

int testSingleFile(const QString& path, bool metadataOnly)
{
  using namespace Deuterium::Gig;

  auto meta = loadGigFileMetadata(path);
  if(!meta)
    return Unloadable;

  const int instruments = (int)meta->instruments.size();
  bool anyPlayable = metadataOnly;
  for(int i = 0; i < instruments; i++)
  {
    auto m = (i == 0) ? meta : loadGigFileMetadata(path, i);
    if(!m)
      return Unloadable;

    if(!metadataOnly)
    {
      // Samples are decoded instrument by instrument; each result is
      // released before the next one so peak memory stays bounded by the
      // largest single instrument.
      auto loaded = loadGigFileSamples(m, 48000, {});
      if(!loaded)
        return LoadFailed;
      // Individual empty instruments are common in real banks (bank
      // variation presets etc.); only a file with no playable instrument
      // at all is a failure
      auto& instr = loaded->instruments[loaded->selectedInstrument];
      if(!instr.regions.empty())
        anyPlayable = true;

      // Playback validation of every region
      for(const auto& region : instr.regions)
        if(!playbackRegionOk(region))
          return BadPlayback;
    }
  }
  return anyPlayable ? Ok : EmptyLoad;
}

struct Result
{
  QString file;
  enum
  {
    Passed,
    Failed,     // Unloadable / LoadFailed
    Crashed,
    TimedOut
  } status{};
  int exitCode{};
};

const char* statusString(const Result& r)
{
  switch(r.status)
  {
    case Result::Passed:
      return "PASS ";
    case Result::Failed:
      return r.exitCode == Unloadable    ? "NOLOAD"
             : r.exitCode == EmptyLoad   ? "EMPTY"
             : r.exitCode == BadPlayback ? "BADPLAY"
                                         : "NOSMP";
    case Result::Crashed:
      return "CRASH";
    case Result::TimedOut:
      return "TIMEOUT";
  }
  return "?";
}
}

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);

  QStringList paths;
  bool metadataOnly = false;
  QString singleFile;
  int jobs = 4;
  int timeoutSecs = 600;

  const QStringList args = app.arguments();
  for(int i = 1; i < args.size(); i++)
  {
    if(args[i] == "--single" && i + 1 < args.size())
      singleFile = args[++i];
    else if(args[i] == "--metadata-only")
      metadataOnly = true;
    else if(args[i] == "-j" && i + 1 < args.size())
      jobs = std::max(1, args[++i].toInt());
    else if(args[i] == "--timeout" && i + 1 < args.size())
      timeoutSecs = std::max(1, args[++i].toInt());
    else
      paths.push_back(args[i]);
  }

  // Child mode: test one file in this process
  if(!singleFile.isEmpty())
    return testSingleFile(singleFile, metadataOnly);

  if(paths.empty())
    paths.push_back(
        QDir::homePath() + "/Documents/ossia/score/packages/samples");

  // Collect the files to test
  QStringList files;
  for(const auto& p : paths)
  {
    QFileInfo fi{p};
    if(fi.isFile())
    {
      files.push_back(fi.absoluteFilePath());
    }
    else if(fi.isDir())
    {
      QDirIterator it{
          p,
          {"*.gig", "*.GIG", "*.dls", "*.DLS", "*.sf2", "*.SF2", "*.kmp",
           "*.KMP", "drumkit.xml"},
          QDir::Files,
          QDirIterator::Subdirectories};
      while(it.hasNext())
        files.push_back(it.next());
    }
  }
  files.sort();

  if(files.empty())
  {
    std::fprintf(stderr, "No sample files found in the given paths; nothing to test.\n");
    return 0;
  }

  std::printf(
      "Testing %d files (%s, %d jobs, %ds timeout)...\n", (int)files.size(),
      metadataOnly ? "metadata only" : "full sample decode", jobs, timeoutSecs);

  const QString self = QCoreApplication::applicationFilePath();
  std::atomic<int> next{0};
  std::atomic<int> done{0};
  std::mutex resultsMutex;
  std::vector<Result> results;

  auto worker = [&] {
    for(;;)
    {
      const int i = next.fetch_add(1);
      if(i >= files.size())
        return;
      const QString& file = files[i];

      QStringList childArgs{"--single", file};
      if(metadataOnly)
        childArgs.push_back("--metadata-only");

      QProcess proc;
      proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
      proc.start(self, childArgs);

      Result r;
      r.file = file;
      if(!proc.waitForFinished(timeoutSecs * 1000))
      {
        proc.kill();
        proc.waitForFinished(5000);
        r.status = Result::TimedOut;
      }
      else if(proc.exitStatus() == QProcess::CrashExit)
      {
        r.status = Result::Crashed;
      }
      else if(proc.exitCode() != Ok)
      {
        r.status = Result::Failed;
        r.exitCode = proc.exitCode();
      }
      else
      {
        r.status = Result::Passed;
      }

      std::lock_guard lock{resultsMutex};
      results.push_back(r);
      std::printf(
          "[%4d/%4d] %-7s %s\n", ++done, (int)files.size(), statusString(r),
          qPrintable(file));
      std::fflush(stdout);
    }
  };

  std::vector<std::thread> threads;
  for(int i = 0; i < jobs; i++)
    threads.emplace_back(worker);
  for(auto& t : threads)
    t.join();

  int passed{}, failed{}, crashed{}, timedOut{};
  for(const auto& r : results)
  {
    switch(r.status)
    {
      case Result::Passed:
        passed++;
        break;
      case Result::Failed:
        failed++;
        break;
      case Result::Crashed:
        crashed++;
        break;
      case Result::TimedOut:
        timedOut++;
        break;
    }
  }

  std::printf(
      "\nSummary: %d passed, %d failed to load, %d crashed, %d timed out\n", passed,
      failed, crashed, timedOut);
  for(const auto& r : results)
    if(r.status != Result::Passed)
      std::printf("  %-7s %s\n", statusString(r), qPrintable(r.file));

  return (crashed + timedOut) > 0 ? 1 : 0;
}
