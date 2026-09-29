// The sampler's bank through the project file operations. A Hydrogen
// drumkit.xml names its samples relative to itself: consolidating, archiving
// and cleaning up a project have to treat the kit as the drumkit.xml together
// with those samples, and the sampler has to find the kit again through the
// <PROJECT>: path consolidating leaves in the document.

#include <Process/ProjectArchive.hpp>
#include <Process/ProjectConsolidation.hpp>
#include <Process/UnusedFiles.hpp>

#include <score/tools/ProjectFiles.hpp>

#include <core/presenter/DocumentManager.hpp>

#include <Deuterium/GigSampler/ProcessModel.hpp>

#include <ossia/detail/algorithms.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Document.hpp>
#include <score_test/Events.hpp>
#include <score_test/Process.hpp>
#include <score_test/Project.hpp>

namespace
{
const QString sampler_uuid = QStringLiteral("95f8ee65-e418-4f75-b5e4-3e039bb90ac8");

//! A two-instrument kit whose samples sit next to its drumkit.xml, one of them
//! in a sub-folder.
void write_kit(const QString& folder)
{
  score::test::write_wav(folder + "/kick.wav", 0.1, 0x21);
  score::test::write_wav(folder + "/layers/snare.wav", 0.1, 0x42);
  score::test::write_file(folder + "/drumkit.xml", R"_(<?xml version="1.0" encoding="UTF-8"?>
<drumkit_info>
  <name>ConsolidationKit</name>
  <instrumentList>
    <instrument>
      <id>0</id>
      <name>Kick</name>
      <midiOutNote>36</midiOutNote>
      <filename>kick.wav</filename>
    </instrument>
    <instrument>
      <id>1</id>
      <name>Snare</name>
      <midiOutNote>38</midiOutNote>
      <filename>layers/snare.wav</filename>
    </instrument>
  </instrumentList>
</drumkit_info>
)_");
}

Deuterium::Gig::ProcessModel& as_sampler(Process::ProcessModel& proc)
{
  // The addon's types are not exported from the plug-in: no RTTI across it
  REQUIRE(
      proc.concreteKey() == Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get());
  return static_cast<Deuterium::Gig::ProcessModel&>(proc);
}

//! Waits for the sampler's load and returns the files its regions play.
std::vector<QString> loaded_samples(Deuterium::Gig::ProcessModel& sampler)
{
  REQUIRE(score::test::wait_until([&] { return sampler.gigInfo() != nullptr; }));
  std::vector<QString> files;
  for(const auto& instrument : sampler.gigInfo()->instruments)
    for(const auto& region : instrument.regions)
    {
      CHECK(region.sample.data);
      files.push_back(QString::fromStdString(region.sample.sourceFile));
    }
  return files;
}
}

TEST_CASE("Deuterium: a drumkit is consolidated with its samples", "[deuterium]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir projectDir, mediaDir;
    REQUIRE(projectDir.isValid());
    REQUIRE(mediaDir.isValid());
    const QString project = score::test::canonical(projectDir.path());
    const QString media = score::test::canonical(mediaDir.path());

    write_kit(media + "/MyKit");

    auto* doc = score::test::project_document(ctx, project, "kit.score");
    auto* proc = score::test::add_process(*doc, sampler_uuid, media + "/MyKit/drumkit.xml");
    REQUIRE(proc);
    auto& sampler = as_sampler(*proc);
    CHECK(loaded_samples(sampler).size() == 2);

    const auto report = Process::consolidateProjectFiles(doc->context(), {});
    const auto* kit = score::test::entry_for(report, "drumkit.xml");
    REQUIRE(kit);
    CHECK(kit->action == Process::FileAction::Collect);
    CHECK(kit->destinationPath == project + "/Data/MyKit/drumkit.xml");
    CHECK(kit->note.isEmpty());

    // The kit keeps its layout, in a folder of its own.
    CHECK(score::sameFileContents(
        media + "/MyKit/kick.wav", project + "/Data/MyKit/kick.wav"));
    CHECK(score::sameFileContents(
        media + "/MyKit/layers/snare.wav", project + "/Data/MyKit/layers/snare.wav"));
    CHECK(
        score::test::control_string(score::test::control_named(*proc, "File"))
        == "<PROJECT>:Data/MyKit/drumkit.xml");

    // Nothing of the kit is proposed for removal...
    const auto unused = Process::analyzeUnusedFiles(doc->context(), {});
    CHECK(unused.count(Process::FileAction::Unused) == 0);

    // ... and all of it goes into an archive.
    std::vector<QString> archived;
    for(const auto& e : Process::projectArchiveContents(doc->context(), report))
      archived.push_back(e.nameInArchive);
    CHECK(ossia::contains(archived, QStringLiteral("kit/Data/MyKit/drumkit.xml")));
    CHECK(ossia::contains(archived, QStringLiteral("kit/Data/MyKit/kick.wav")));
    CHECK(ossia::contains(archived, QStringLiteral("kit/Data/MyKit/layers/snare.wav")));

    // Consolidating again finds everything in place.
    const auto again = Process::consolidateProjectFiles(doc->context(), {});
    CHECK(again.bytesToCopy() == 0);
    CHECK(score::test::entry_for(again, "drumkit.xml")->action
          == Process::FileAction::AlreadyThere);

    // The project stands on its own: without the original kit, the reopened
    // document still plays the collected one.
    const auto key = proc->concreteKey();
    REQUIRE(ctx.docManager.saveDocument(*doc));
    REQUIRE(QDir{media + "/MyKit"}.removeRecursively());
    ctx.docManager.forceCloseDocument(ctx, *doc);
    score::test::settle();

    auto* reopened = ctx.docManager.loadFile(ctx, project + "/kit.score");
    REQUIRE(reopened);
    Process::ProcessModel* reloaded{};
    for(auto& p : score::test::base_interval(*reopened).processes)
      if(p.concreteKey() == key)
        reloaded = &p;
    REQUIRE(reloaded);

    const auto files = loaded_samples(as_sampler(*reloaded));
    REQUIRE(files.size() == 2);
    for(const auto& f : files)
      CHECK(score::isUnderFolder(f, project + "/Data/MyKit"));
  });
}
