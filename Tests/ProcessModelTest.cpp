// Unit tests for Deuterium::Gig::ProcessModel: construction must never throw
// and the file path must survive serialization even when the file is missing.
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/TimeValue.hpp>

#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>

#include <core/application/MinimalApplication.hpp>

#include "TestHelpers.hpp"

#include <QCoreApplication>
#include <QFile>
#include <gig.h>
#include <QThread>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <Dataflow/AudioOutletItem.hpp>
#include <Dataflow/MidiInletItem.hpp>
#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>

static score::MinimalApplication* g_app{};

// The application is shared by every test case, so it is torn down once the
// run is over, like score's own application tests do: otherwise
// LeakSanitizer reports everything it owns (notably the JS plugin's
// QJSEngine, whose heap LSan cannot scan) as leaked at exit.
class AppTeardown final : public Catch::EventListenerBase
{
public:
  using Catch::EventListenerBase::EventListenerBase;
  void testRunEnded(const Catch::TestRunStats&) override
  {
    if(!g_app)
      return;
    QCoreApplication::processEvents();
    delete g_app;
    g_app = nullptr;
  }
};
CATCH_REGISTER_LISTENER(AppTeardown)

// Entity construction reaches score::AppContext() (through score::Skin), so
// a real application context is required; MinimalApplication also loads the
// plugins, providing the factories used by writePorts(). Called at the top
// of every test case.
static void bootApp()
{
  static bool booted = [] {
    if(!qEnvironmentVariableIsSet("DISPLAY")
       && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
      qputenv("QT_QPA_PLATFORM", "offscreen");

    // Destroyed by AppTeardown once every test case has run
    g_app = new score::MinimalApplication;
    auto& app = *g_app;

    // writePorts() resolves the ports through the PortFactoryList; make sure
    // the factories exist even if the plugins were not found on disk.
    auto& factories = app.componentsData().factories;
    auto it = factories.find(Process::PortFactory::static_interfaceKey());
    if(it == factories.end())
    {
      auto pl = std::make_unique<Process::PortFactoryList>();
      pl->insert(std::make_unique<Dataflow::MidiInletFactory>());
      pl->insert(std::make_unique<Dataflow::AudioOutletFactory>());
      factories.insert(
          {Process::PortFactory::static_interfaceKey(), std::move(pl)});
    }
    else
    {
      auto& pl = static_cast<Process::PortFactoryList&>(*it->second);
      if(!pl.get(Dataflow::MidiInletFactory{}.concreteKey()))
        pl.insert(std::make_unique<Dataflow::MidiInletFactory>());
      if(!pl.get(Dataflow::AudioOutletFactory{}.concreteKey()))
        pl.insert(std::make_unique<Dataflow::AudioOutletFactory>());
    }
    return true;
  }();
  (void)booted;
}

TEST_CASE("model: constructor_does_not_throw_on_missing_file", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), "/nonexistent/dir/missing.gig",
      Id<Process::ProcessModel>{1}, nullptr};

  REQUIRE(p.effect() == QStringLiteral("/nonexistent/dir/missing.gig"));
  REQUIRE(!p.gigInfo());
  REQUIRE(p.inlets().size() == std::size_t(1 + Deuterium::Gig::ControlCount));
  REQUIRE(p.outlets().size() == std::size_t(1));
}

TEST_CASE("model: constructor_does_not_throw_on_empty_string", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{2}, nullptr};

  REQUIRE(p.effect() == QString{});
  REQUIRE(!p.gigInfo());
  REQUIRE(p.inlets().size() == std::size_t(1 + Deuterium::Gig::ControlCount));
  REQUIRE(p.outlets().size() == std::size_t(1));
}

// The process must keep the UUID of the Deuterium released in earlier
// ossia score versions, so their documents resolve to this implementation.
TEST_CASE("model: uuid_is_the_released_deuterium", "[deuterium]")
{
  bootApp();
  const auto& key = Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get();
  const auto expected = UuidKey<Process::ProcessModel>::fromString(
      std::string("95f8ee65-e418-4f75-b5e4-3e039bb90ac8"));
  REQUIRE(key == expected);
}

// A document saved by the released drumkit-only Deuterium: JSON field
// "Kit" instead of "File"/"Instrument", and only the MIDI inlet + audio
// outlet (no control inlets). It must load, keep the kit path, and gain
// the full control set.
TEST_CASE("model: released_deuterium_document_loads", "[deuterium]")
{
  bootApp();
  const QString path = "/nonexistent/kits/GMkit/drumkit.xml";
  Deuterium::Gig::ProcessModel current{
      TimeVal::fromMsecs(1000), path, Id<Process::ProcessModel>{20}, nullptr};

  auto doc = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(current)));

  // Shape the document like the legacy format
  doc.RemoveMember("File");
  doc.RemoveMember("Instrument");
  doc.AddMember("Kit", rapidjson::Value{path.toUtf8(), doc.GetAllocator()},
                doc.GetAllocator());
  auto& inlets = doc["Inlets"];
  REQUIRE(inlets.IsArray());
  REQUIRE(inlets.Size() > 1);
  inlets.Erase(inlets.Begin() + 1, inlets.End()); // keep only the MIDI inlet

  JSONObject::Deserializer des{doc};
  Deuterium::Gig::ProcessModel loaded{des, nullptr};

  REQUIRE(loaded.effect() == path);
  REQUIRE(loaded.instrument() == 0);
  // The control inlets missing from the legacy document were created
  REQUIRE(loaded.inlets().size() == std::size_t(1 + Deuterium::Gig::ControlCount));
  REQUIRE((qobject_cast<Process::MidiInlet*>(loaded.inlets()[0])));
  REQUIRE((qobject_cast<Process::ControlInlet*>(
      loaded.inlets()[1 + Deuterium::Gig::Volume])));

  // And a re-save uses the current format while keeping the path
  auto doc2 = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(loaded)));
  REQUIRE(doc2.HasMember("File"));
  REQUIRE(!doc2.HasMember("Kit"));
  JSONObject::Deserializer des2{doc2};
  Deuterium::Gig::ProcessModel loaded2{des2, nullptr};
  REQUIRE(loaded2.effect() == path);
  REQUIRE(loaded2.inlets().size() == std::size_t(1 + Deuterium::Gig::ControlCount));
}

// A document saved before the Note off and MIDI channel controls: they are
// appended with the behaviour the document was made with (note-offs release,
// every channel plays), and a re-save keeps them.
TEST_CASE("model: document_without_note_off_and_channel_loads", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel current{
      TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{21}, nullptr};
  auto doc = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(current)));
  auto& inlets = doc["Inlets"];
  REQUIRE(inlets.Size() == 1 + Deuterium::Gig::ControlCount);
  inlets.Erase(inlets.Begin() + 1 + Deuterium::Gig::NoteOff, inlets.End());

  JSONObject::Deserializer des{doc};
  Deuterium::Gig::ProcessModel loaded{des, nullptr};
  REQUIRE(loaded.inlets().size() == std::size_t(1 + Deuterium::Gig::ControlCount));
  auto control = [&](int c) {
    auto* ctl = qobject_cast<Process::ControlInlet*>(loaded.inlets()[1 + c]);
    REQUIRE(ctl);
    return ctl;
  };
  CHECK(control(Deuterium::Gig::NoteOff)->value() == ossia::value{false});
  CHECK(ossia::convert<int>(control(Deuterium::Gig::MidiChannel)->value()) == 0);

  control(Deuterium::Gig::NoteOff)->setValue(true);
  control(Deuterium::Gig::MidiChannel)->setValue(10);
  auto doc2 = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(loaded)));
  JSONObject::Deserializer des2{doc2};
  Deuterium::Gig::ProcessModel loaded2{des2, nullptr};
  auto* noteOff = qobject_cast<Process::ControlInlet*>(
      loaded2.inlets()[1 + Deuterium::Gig::NoteOff]);
  auto* channel = qobject_cast<Process::ControlInlet*>(
      loaded2.inlets()[1 + Deuterium::Gig::MidiChannel]);
  REQUIRE(noteOff);
  REQUIRE(channel);
  CHECK(noteOff->value() == ossia::value{true});
  CHECK(ossia::convert<int>(channel->value()) == 10);
}

// A combobox saves its entries in the document: one saved before entries
// were added (Voices before the fingered modes) gets the current entries on
// load, and keeps its value
TEST_CASE("model: older_combobox_entries_are_refreshed", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel current{
      TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{22}, nullptr};
  auto* voices = qobject_cast<Process::ComboBox*>(
      current.inlets()[1 + Deuterium::Gig::VoiceMode]);
  REQUIRE(voices);
  const auto entries = voices->alternatives;
  REQUIRE(entries.size() == 8);
  voices->setAlternatives({entries.begin(), entries.begin() + 3});
  voices->setValue(2); // Legato
  REQUIRE(voices->alternatives.size() == 3);

  auto doc = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(current)));
  JSONObject::Deserializer des{doc};
  Deuterium::Gig::ProcessModel loaded{des, nullptr};
  auto* restored = qobject_cast<Process::ComboBox*>(
      loaded.inlets()[1 + Deuterium::Gig::VoiceMode]);
  REQUIRE(restored);
  CHECK(restored->alternatives == entries);
  CHECK(ossia::convert<int>(restored->value()) == 2);
}

// The "<path>|<instrument>" creation string is split at construction and
// the instrument index survives serialization as its own field, so saved
// documents do not depend on filesystem state to parse the reference.
TEST_CASE("model: instrument_index_roundtrip", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), "/nonexistent/dir/bank.sf2|2",
      Id<Process::ProcessModel>{10}, nullptr};

  REQUIRE(p.effect() == QStringLiteral("/nonexistent/dir/bank.sf2"));
  REQUIRE(p.instrument() == 2);

  const auto doc
      = toValue(score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(p)));
  JSONObject::Deserializer des{doc};
  Deuterium::Gig::ProcessModel loaded{des, nullptr};
  REQUIRE(loaded.effect() == QStringLiteral("/nonexistent/dir/bank.sf2"));
  REQUIRE(loaded.instrument() == 2);
}

TEST_CASE("model: loadFile_keeps_path_on_failure", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{3}, nullptr};

  p.loadFile("/nonexistent/dir/missing.sf2");
  REQUIRE(p.effect() == QStringLiteral("/nonexistent/dir/missing.sf2"));
  REQUIRE(!p.gigInfo());
}

// Process models are serialized polymorphically through their base class;
// marshalling through the base reference writes the entity data + uuid +
// the concrete class's data, which is what the deserialization constructor
// consumes for JSON.
TEST_CASE("model: json_roundtrip_keeps_missing_path", "[deuterium]")
{
  bootApp();
  const QString path = "/nonexistent/dir/missing.gig";
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), path, Id<Process::ProcessModel>{4}, nullptr};

  const auto doc
      = toValue(score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(p)));
  JSONObject::Deserializer des{doc};
  Deuterium::Gig::ProcessModel loaded{des, nullptr};

  // The reference to the (currently unavailable) file must not be erased:
  // saving again right away has to keep pointing to it.
  REQUIRE(loaded.effect() == path);

  const auto doc2 = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(loaded)));
  JSONObject::Deserializer des2{doc2};
  Deuterium::Gig::ProcessModel loaded2{des2, nullptr};
  REQUIRE(loaded2.effect() == path);
}

// DataStream is positional, so exercise the concrete reader/writer pair
// (what document save / load runs for this process) directly.
TEST_CASE("model: datastream_write_keeps_missing_path", "[deuterium]")
{
  bootApp();
  const QString path = "/nonexistent/dir/missing.dls";
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), path, Id<Process::ProcessModel>{5}, nullptr};

  QByteArray arr;
  DataStream::Serializer ser{&arr};
  ser.read(p);

  Deuterium::Gig::ProcessModel target{
      TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{6}, nullptr};
  DataStream::Deserializer des{arr};
  des.write(target);

  REQUIRE(target.effect() == path);
  REQUIRE(!target.gigInfo());
}

// Timing-related controls use Process::TimeChooser so they can be tempo-
// synced; the cutoff uses a log-scale slider.
TEST_CASE("model: control_widget_types", "[deuterium]")
{
  bootApp();
  QObject parent;
  auto controls = Deuterium::Gig::makeSamplerControls(&parent);
  REQUIRE(controls.size() == std::size_t(Deuterium::Gig::ControlCount));

  for(int i = 0; i < Deuterium::Gig::ControlCount; i++)
  {
    if(Deuterium::Gig::isTimeControl(i))
      REQUIRE((dynamic_cast<Process::TimeChooser*>(controls[i])));
    else
      REQUIRE(!(dynamic_cast<Process::TimeChooser*>(controls[i])));
  }
  REQUIRE((dynamic_cast<Process::LogFloatSlider*>(
      controls[Deuterium::Gig::Cutoff])));

  // TimeChooser default: free-running mode with the initial time in seconds
  auto* lfoRate = controls[Deuterium::Gig::LfoRate];
  auto v = ossia::convert<ossia::vec2f>(lfoRate->value());
  REQUIRE(approxEq(v[0], 0.2f));
  REQUIRE(approxEq(v[1], 0.f)); // widget convention: y == 0 is free-running
}

// The Instrument inlet and the serialized instrument index stay in sync:
// the port drives which instrument plays without touching other controls.
TEST_CASE("model: instrument_port_sync", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), "/nonexistent/dir/bank.sf2|3",
      Id<Process::ProcessModel>{30}, nullptr};

  auto* ctl = qobject_cast<Process::ControlInlet*>(
      p.inlets()[1 + Deuterium::Gig::Instrument]);
  REQUIRE(ctl);
  REQUIRE(ossia::convert<int>(ctl->value()) == 3);

  // Changing the port changes the instrument, keeps the file path
  ctl->setValue(5);
  REQUIRE(p.instrument() == 5);
  REQUIRE(p.effect() == QStringLiteral("/nonexistent/dir/bank.sf2"));

  // loadFile keeps the port in sync
  p.loadFile("/nonexistent/dir/other.gig", 1);
  REQUIRE(ossia::convert<int>(ctl->value()) == 1);
  REQUIRE(p.instrument() == 1);

  // And the legacy-document append path gives the toggle+port sensible
  // defaults (checked in released_deuterium_document_loads via ControlCount)
}

// Switching instrument while a load is running must stop it. Without this the
// old load kept one of the two-to-four TaskPool threads busy decoding samples
// nobody wanted, and then published them over the instrument just asked for.
TEST_CASE("model: starting a load cancels the one in flight", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), "/nonexistent/dir/missing.gig",
      Id<Process::ProcessModel>{1}, nullptr};

  p.loadFile("/nonexistent/dir/a.gig", 0);
  auto first = p.currentLoadToken();
  REQUIRE(first);
  CHECK_FALSE(first->load(std::memory_order_relaxed));

  p.loadFile("/nonexistent/dir/b.gig", 0);
  auto second = p.currentLoadToken();
  REQUIRE(second);

  // The one that was running is told to stop, and the new one is its own.
  CHECK(first->load(std::memory_order_relaxed));
  CHECK(first != second);
  CHECK_FALSE(second->load(std::memory_order_relaxed));
}

TEST_CASE("model: switching instrument cancels the load in flight", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel p{
      TimeVal::fromMsecs(1000), "/nonexistent/dir/missing.gig",
      Id<Process::ProcessModel>{1}, nullptr};

  p.loadFile("/nonexistent/dir/a.gig", 0);
  auto first = p.currentLoadToken();
  REQUIRE(first);
  CHECK_FALSE(first->load(std::memory_order_relaxed));

  // Drive the port the instrument combo drives, which reaches startAsyncLoad
  // directly rather than through loadFile.
  auto* inlet = safe_cast<Process::ControlInlet*>(
      p.inlets()[1 + Deuterium::Gig::Instrument]);
  REQUIRE(inlet);
  inlet->setValue(1);

  CHECK(p.instrument() == 1);
  CHECK(first->load(std::memory_order_relaxed));
  CHECK(p.currentLoadToken() != first);
}

// The bend range a gig bank specifies becomes the Bend range control when
// the user creates the process or picks the bank; a restored document keeps
// its own value
TEST_CASE("model: the bank's bend range on user loads only", "[deuterium]")
{
  bootApp();
  QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("bend.gig"));
  {
    gig::File f;
    std::vector<int16_t> data(400, 1000);
    gig::Sample* smp = f.AddSample();
    smp->Channels = 1;
    smp->BitDepth = 16;
    smp->FrameSize = 2;
    smp->SamplesPerSecond = 44100;
    smp->Resize(data.size());
    gig::Instrument* ins = f.AddInstrument();
    ins->PitchbendRange = 7;
    gig::Region* rgn = ins->AddRegion();
    rgn->SetSample(smp);
    rgn->pDimensionRegions[0]->pSample = smp;
    f.Save(path.toStdString());
    smp->SetPos(0);
    smp->Write(data.data(), data.size());
  }
  // Until the load in flight has landed (fileChanged)
  auto waitLoaded = [](Deuterium::Gig::ProcessModel& p) {
    bool landed = false;
    auto c = QObject::connect(
        &p, &Deuterium::Gig::ProcessModel::fileChanged, [&] { landed = true; });
    QElapsedTimer t;
    t.start();
    while(!landed && t.elapsed() < 10000)
    {
      QCoreApplication::processEvents();
      QThread::msleep(5);
    }
    QObject::disconnect(c);
    return landed && p.gigInfo() != nullptr;
  };
  auto bend = [](Deuterium::Gig::ProcessModel& p) {
    auto* ctl = qobject_cast<Process::ControlInlet*>(
        p.inlets()[1 + Deuterium::Gig::BendRange]);
    REQUIRE(ctl);
    return ossia::convert<float>(ctl->value());
  };

  Deuterium::Gig::ProcessModel created{
      TimeVal::fromMsecs(1000), path, Id<Process::ProcessModel>{40}, nullptr};
  REQUIRE(waitLoaded(created));
  CHECK(bend(created) == 7.f);

  // Another bank, the range untouched: it follows the bank
  const QString other = dir.filePath(QStringLiteral("bend9.gig"));
  QFile::copy(path, other);
  {
    RIFF::File riff(other.toStdString());
    gig::File f(&riff);
    f.GetInstrument(0)->PitchbendRange = 9;
    f.Save();
  }
  auto* fileCtl = qobject_cast<Process::ControlInlet*>(
      created.inlets()[1 + Deuterium::Gig::File]);
  REQUIRE(fileCtl);
  fileCtl->setValue(other.toStdString());
  REQUIRE(waitLoaded(created));
  CHECK(bend(created) == 9.f);

  // A range the user chose is kept when the bank changes (or an undo
  // brings the previous one back)
  qobject_cast<Process::ControlInlet*>(
      created.inlets()[1 + Deuterium::Gig::BendRange])
      ->setValue(5.f);
  fileCtl->setValue(path.toStdString());
  REQUIRE(waitLoaded(created));
  CHECK(bend(created) == 5.f);

  // The user sets another range, the document is saved and reopened
  qobject_cast<Process::ControlInlet*>(
      created.inlets()[1 + Deuterium::Gig::BendRange])
      ->setValue(3.f);
  auto doc = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(created)));
  JSONObject::Deserializer des{doc};
  Deuterium::Gig::ProcessModel restored{des, nullptr};
  REQUIRE(waitLoaded(restored));
  CHECK(bend(restored) == 3.f);
}

// Documents from before Mono fade / Vel curve existed get the behaviour they
// were made with: a 0.3 s fade of the replaced mono note, the hard curve
TEST_CASE("model: older documents keep their mono fade and velocity curve", "[deuterium]")
{
  bootApp();
  Deuterium::Gig::ProcessModel current{
      TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{41}, nullptr};
  auto doc = toValue(
      score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(current)));
  auto& inlets = doc["Inlets"];
  // A document predating every control from VelCurve on
  inlets.Erase(inlets.Begin() + 1 + Deuterium::Gig::VelCurve, inlets.End());
  JSONObject::Deserializer des{doc};
  Deuterium::Gig::ProcessModel loaded{des, nullptr};
  auto control = [&](int c) {
    auto* ctl = qobject_cast<Process::ControlInlet*>(loaded.inlets()[1 + c]);
    REQUIRE(ctl);
    return ctl->value();
  };
  CHECK(ossia::convert<int>(control(Deuterium::Gig::VelCurve)) == 2);
  const auto fade = ossia::convert<ossia::vec2f>(control(Deuterium::Gig::MonoFade));
  CHECK(std::abs(fade[0] - 0.3f) < 1e-6);
  // A new process has the new defaults
  auto* fresh = qobject_cast<Process::ControlInlet*>(
      current.inlets()[1 + Deuterium::Gig::VelCurve]);
  CHECK(ossia::convert<int>(fresh->value()) == 3);
}
