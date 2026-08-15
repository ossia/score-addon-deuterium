// Unit tests for Deuterium::Gig::ProcessModel: construction must never throw
// and the file path must survive serialization even when the file is missing.
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/TimeValue.hpp>

#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>

#include <core/application/MinimalApplication.hpp>

#include "TestHelpers.hpp"

#include <Dataflow/AudioOutletItem.hpp>
#include <Dataflow/MidiInletItem.hpp>
#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>

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

    // Intentionally leaked: tearing the application down unloads every plugin,
    // and some unrelated plugins do not survive their destructors; the process
    // exits right after the tests anyway.
    auto& app = *new score::MinimalApplication;

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
