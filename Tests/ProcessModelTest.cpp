// Unit tests for Deuterium::Gig::ProcessModel: construction must never throw
// and the file path must survive serialization even when the file is missing.
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/TimeValue.hpp>

#include <score/model/EntitySerialization.hpp>
#include <score/serialization/VisitorCommon.hpp>

#include <core/application/MinimalApplication.hpp>

#include <QtTest>

#include <Dataflow/AudioOutletItem.hpp>
#include <Dataflow/MidiInletItem.hpp>
#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>

class ProcessModelTest final : public QObject
{
  Q_OBJECT

  // Entity construction reaches score::AppContext() (through score::Skin), so
  // a real application context is required; MinimalApplication also loads the
  // plugins, providing the factories used by writePorts().
  struct EnvSetup
  {
    EnvSetup()
    {
      if(!qEnvironmentVariableIsSet("DISPLAY")
         && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
  } m_env;
  // Intentionally leaked: tearing the application down unloads every plugin,
  // and some unrelated plugins do not survive their destructors; the process
  // exits right after the tests anyway.
  score::MinimalApplication& m_app = *new score::MinimalApplication;

public:
  ProcessModelTest()
  {
    // writePorts() resolves the ports through the PortFactoryList; make sure
    // the factories exist even if the plugins were not found on disk.
    auto& factories = m_app.componentsData().factories;
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
  }

private Q_SLOTS:

  void test_constructor_does_not_throw_on_missing_file()
  {
    Deuterium::Gig::ProcessModel p{
        TimeVal::fromMsecs(1000), "/nonexistent/dir/missing.gig",
        Id<Process::ProcessModel>{1}, nullptr};

    QCOMPARE(p.effect(), QStringLiteral("/nonexistent/dir/missing.gig"));
    QVERIFY(!p.gigInfo());
    QCOMPARE(p.inlets().size(), std::size_t(1 + Deuterium::Gig::ControlCount));
    QCOMPARE(p.outlets().size(), std::size_t(1));
  }

  void test_constructor_does_not_throw_on_empty_string()
  {
    Deuterium::Gig::ProcessModel p{
        TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{2}, nullptr};

    QCOMPARE(p.effect(), QString{});
    QVERIFY(!p.gigInfo());
    QCOMPARE(p.inlets().size(), std::size_t(1 + Deuterium::Gig::ControlCount));
    QCOMPARE(p.outlets().size(), std::size_t(1));
  }

  // The "<path>|<instrument>" creation string is split at construction and
  // the instrument index survives serialization as its own field, so saved
  // documents do not depend on filesystem state to parse the reference.
  void test_instrument_index_roundtrip()
  {
    Deuterium::Gig::ProcessModel p{
        TimeVal::fromMsecs(1000), "/nonexistent/dir/bank.sf2|2",
        Id<Process::ProcessModel>{10}, nullptr};

    QCOMPARE(p.effect(), QStringLiteral("/nonexistent/dir/bank.sf2"));
    QCOMPARE(p.instrument(), 2);

    const auto doc
        = toValue(score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(p)));
    JSONObject::Deserializer des{doc};
    Deuterium::Gig::ProcessModel loaded{des, nullptr};
    QCOMPARE(loaded.effect(), QStringLiteral("/nonexistent/dir/bank.sf2"));
    QCOMPARE(loaded.instrument(), 2);
  }

  void test_loadFile_keeps_path_on_failure()
  {
    Deuterium::Gig::ProcessModel p{
        TimeVal::fromMsecs(1000), "", Id<Process::ProcessModel>{3}, nullptr};

    p.loadFile("/nonexistent/dir/missing.sf2");
    QCOMPARE(p.effect(), QStringLiteral("/nonexistent/dir/missing.sf2"));
    QVERIFY(!p.gigInfo());
  }

  // Process models are serialized polymorphically through their base class;
  // marshalling through the base reference writes the entity data + uuid +
  // the concrete class's data, which is what the deserialization constructor
  // consumes for JSON.
  void test_json_roundtrip_keeps_missing_path()
  {
    const QString path = "/nonexistent/dir/missing.gig";
    Deuterium::Gig::ProcessModel p{
        TimeVal::fromMsecs(1000), path, Id<Process::ProcessModel>{4}, nullptr};

    const auto doc
        = toValue(score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(p)));
    JSONObject::Deserializer des{doc};
    Deuterium::Gig::ProcessModel loaded{des, nullptr};

    // The reference to the (currently unavailable) file must not be erased:
    // saving again right away has to keep pointing to it.
    QCOMPARE(loaded.effect(), path);

    const auto doc2 = toValue(
        score::marshall<JSONObject>(static_cast<const Process::ProcessModel&>(loaded)));
    JSONObject::Deserializer des2{doc2};
    Deuterium::Gig::ProcessModel loaded2{des2, nullptr};
    QCOMPARE(loaded2.effect(), path);
  }

  // DataStream is positional, so exercise the concrete reader/writer pair
  // (what document save / load runs for this process) directly.
  void test_datastream_write_keeps_missing_path()
  {
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

    QCOMPARE(target.effect(), path);
    QVERIFY(!target.gigInfo());
  }
};

QTEST_APPLESS_MAIN(ProcessModelTest)
#include "ProcessModelTest.moc"
