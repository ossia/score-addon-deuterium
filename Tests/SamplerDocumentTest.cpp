// The sampler in a score document: controls set by name on the process model
// reach the execution node through the executor component, and the node plays
// a file loaded by the process.

#include <Process/Dataflow/Port.hpp>
#include <Process/ExecutionContext.hpp>

#include <Scenario/Document/Interval/IntervalExecution.hpp>

#include <Execution/BaseScenarioComponent.hpp>
#include <Execution/DocumentPlugin.hpp>

#include <Deuterium/GigSampler/ProcessModel.hpp>

#include <ossia/dataflow/exec_state_facade.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>

#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>
#include <libremidi/ump_events.hpp>
#include <score_test/App.hpp>
#include <score_test/Document.hpp>
#include <score_test/Events.hpp>
#include <score_test/Execution.hpp>
#include <score_test/Process.hpp>
#include <score_test/Project.hpp>

#include <cmath>

namespace
{
using libremidi::from_midi1::note_off;
using libremidi::from_midi1::note_on;

struct Rig
{
  Process::ProcessModel& proc;
  Execution::DocumentPlugin& plug;
  std::shared_ptr<ossia::graph_node> node;
  int64_t date{};

  //! Through the model's inlet, as the inspector does: the component forwards
  //! the change to the node on the execution queue.
  void control(const QString& name, const ossia::value& v)
  {
    score::test::control_named(proc, name).setValue(v);
    score::test::run_exec(plug);
  }

  void midi(const libremidi::ump& m)
  {
    node->root_inputs()[0]->target<ossia::midi_port>()->messages.push_back(m);
  }

  double buffer_seconds() const
  {
    const auto& st = *plug.context().execState;
    return double(st.bufferSize) / st.sampleRate;
  }

  //! Runs one buffer; the RMS of the left channel.
  double tick()
  {
    auto& st = *plug.context().execState;
    const int frames = st.bufferSize;
    auto flicks = [&](int64_t s) {
      return ossia::time_value{int64_t(s * ossia::flicks_per_second<double> / st.sampleRate)};
    };
    ossia::token_request tk;
    tk.prev_date = flicks(date);
    tk.date = flicks(date + frames);
    tk.start_sample = 0;
    tk.length_sample = frames;
    date += frames;
    // A new graph tick: the node handles the MIDI of each tick once
    st.samples_since_start += frames;
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    node->run(tk, ossia::exec_state_facade{&st});
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);

    node->root_inputs()[0]->target<ossia::midi_port>()->messages.clear();
    auto& out = *node->root_outputs()[0]->target<ossia::audio_port>();
    double sum = 0.;
    if(out.channels() > 0)
    {
      auto& ch = out.channel(0);
      for(int i = 0; i < frames && i < int(ch.size()); i++)
        sum += ch[i] * ch[i];
      for(auto& c : out.get())
        std::fill(c.begin(), c.end(), 0.);
    }
    REQUIRE(std::isfinite(sum));
    return std::sqrt(sum / frames);
  }

  double run(double seconds)
  {
    double v = 0.;
    for(int i = 0, n = int(std::ceil(seconds / buffer_seconds())); i < n; i++)
      v = tick();
    return v;
  }

  //! Seconds from now to the end of the first buffer quieter than `threshold`.
  double seconds_until_below(double threshold)
  {
    for(int n = 1; n < 100000; n++)
      if(tick() < threshold)
        return n * buffer_seconds();
    FAIL("never below " << threshold);
    return 0.;
  }
};

constexpr double silent = 1e-4;

//! A document with the sampler playing a constant-level wav, its envelope set
//! by `setup` before the execution is created (the component's initial
//! values), and `f` run with the execution in place.
template <typename Setup, typename F>
void with_sampler(Setup&& setup, F&& f, double sampleSeconds = 4.)
{
  score::test::run_in_app([&](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString wav = dir.filePath(QStringLiteral("level.wav"));
    score::test::write_wav(wav, sampleSeconds);

    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* proc = score::test::add_process(
        *doc, QStringLiteral("95f8ee65-e418-4f75-b5e4-3e039bb90ac8"), wav);
    REQUIRE(proc);
    // The addon's types are not exported from the plug-in: no RTTI across it
    REQUIRE(
        proc->concreteKey()
        == Metadata<ConcreteKey_k, Deuterium::Gig::ProcessModel>::get());
    auto& sampler = static_cast<Deuterium::Gig::ProcessModel&>(*proc);
    REQUIRE(score::test::wait_until([&] { return sampler.gigInfo() != nullptr; }));

    Rig r{*proc, doc->context().plugin<Execution::DocumentPlugin>(), {}};
    r.control(QStringLiteral("Envelope source"), false);
    r.control(QStringLiteral("Attack"), ossia::vec2f{0.f, 0.f});
    setup(r);

    r.plug.reload(true, score::test::base_interval(*doc));
    score::test::run_exec(r.plug);
    REQUIRE(r.plug.baseScenario());
    auto& procs = r.plug.baseScenario()->baseInterval().processes();
    auto it = procs.find(proc->id());
    REQUIRE(it != procs.end());
    REQUIRE(it->second->node);
    r.node = it->second->node;

    f(r);
    score::test::settle();
  });
}
}

TEST_CASE("Deuterium in a document: Note off and MIDI channel set by name", "[deuterium]")
{
  with_sampler(
      [](Rig& r) {
    // A note that fades to half its level over one second, then ends in 50 ms
    r.control(QStringLiteral("Decay"), ossia::vec2f{1.f, 0.f});
    r.control(QStringLiteral("Sustain"), 0.5f);
    r.control(QStringLiteral("Release"), ossia::vec2f{0.05f, 0.f});
  },
      [](Rig& r) {
    // Release: the note-off ends the note
    r.midi(note_on(0, 60, 127));
    REQUIRE(r.tick() > 0.01);
    r.midi(note_off(0, 60, 0));
    CHECK(r.run(0.1) < silent);

    // Ignore: the note-off does nothing, the envelope ends the note after its
    // decay
    r.control(QStringLiteral("Note off"), true);
    r.midi(note_on(0, 62, 127));
    REQUIRE(r.tick() > 0.01);
    r.midi(note_off(0, 62, 0));
    CHECK(r.run(0.3) > 0.01);
    CHECK(r.run(1.) < silent);

    // Channel 2 only; channel n is n - 1 on the wire
    r.control(QStringLiteral("Note off"), false);
    r.control(QStringLiteral("MIDI channel"), 2);
    r.midi(note_on(0, 60, 127));
    CHECK(r.tick() < silent);
    r.midi(note_on(1, 62, 127));
    CHECK(r.tick() > 0.01);
    r.midi(note_off(1, 62, 0));
    CHECK(r.run(0.1) < silent);

    // All: every channel, not only the first
    r.control(QStringLiteral("MIDI channel"), 0);
    for(int wire : {9, 15, 0})
    {
      INFO("channel " << wire + 1);
      r.midi(note_on(wire, 64, 127));
      CHECK(r.tick() > 0.01);
      r.midi(note_off(wire, 64, 0));
      CHECK(r.run(0.1) < silent);
    }
  });
}

// With knob envelopes the release falls exponentially from the current level
// to -100 dB (AmpEnv::silence_amp) in exactly the release time R: from a
// sustain of 1 the level is -100 t / R dB at t. The sample is a constant
// level, so a buffer's RMS is the envelope times the held level L. A buffer is
// below L - 40 dB from t = 0.4 R. The note must still sound at 0.4 R minus
// the margin and be silent after 0.4 R plus the margin: 3 dB of envelope
// either side (0.03 R) and one buffer, as the RMS of a buffer averages its
// envelope and time is counted in whole buffers. Ignore mode starts the
// release by itself at the end of the attack (its 1 ms floor), well within
// the margin.
TEST_CASE("Deuterium in a document: release times", "[deuterium]")
{
  with_sampler(
      [](Rig& r) { r.control(QStringLiteral("Sustain"), 1.f); },
      [](Rig& r) {
    const auto check_release = [&](double R, double seconds) {
      const double expected = 0.4 * R;
      const double margin = 0.03 * R + r.buffer_seconds();
      // `seconds` ends the first quiet buffer: every earlier one sounded
      INFO("release " << R << " s: below -40 dB after " << seconds << " s, expected "
                      << expected << " s");
      CHECK(seconds > expected - margin);
      CHECK(seconds < expected + margin);
    };

    // Release: from the note-off, R = 0.5 s -> 0.2 s
    r.control(QStringLiteral("Release"), ossia::vec2f{0.5f, 0.f});
    r.midi(note_on(0, 60, 127));
    r.run(0.1);
    const double held = r.tick();
    REQUIRE(held > 0.01);
    CHECK(std::abs(r.tick() - held) < 1e-3 * held); // sustained
    r.midi(note_off(0, 60, 0));
    check_release(0.5, r.seconds_until_below(held * 0.01));
    CHECK(r.run(0.4) < silent);

    // Ignore: from the note-on, the note-off changes nothing, R = 3 s -> 1.2 s
    r.control(QStringLiteral("Note off"), true);
    r.control(QStringLiteral("Release"), ossia::vec2f{3.f, 0.f});
    r.midi(note_on(0, 60, 127));
    REQUIRE(r.tick() > 0.01);
    r.midi(note_off(0, 60, 0));
    check_release(3., r.buffer_seconds() + r.seconds_until_below(held * 0.01));
    CHECK(r.run(2.) < silent);
  });
}

// Ignore must not turn a note into a one-shot, which does not loop: a sampled
// piano is a short attack and a loop, faded out by its envelope.
TEST_CASE("Deuterium in a document: ignored note-offs keep the sample's loop", "[deuterium]")
{
  with_sampler(
      [](Rig& r) {
    r.control(QStringLiteral("Loop"), 2); // Forward
    r.control(QStringLiteral("Sustain"), 0.f);
    // From full level to -100 dB in exactly 1 s: -30 dB at 0.3 s
    r.control(QStringLiteral("Decay"), ossia::vec2f{1.f, 0.f});
    r.control(QStringLiteral("Note off"), true);
  },
      [](Rig& r) {
    r.midi(note_on(0, 60, 127));
    const double start = r.tick();
    REQUIRE(start > 0.01);
    r.midi(note_off(0, 60, 0));
    // Well past the end of the 0.1 s sample, the loop still sounds
    CHECK(r.run(0.3) > start * 0.01);
    // and the decay ends it
    CHECK(r.run(0.8) < silent);
  },
      0.1);
}

// Each note keeps the note-off mode it started in, so a note held while the
// mode switches between Release and Ignore does not get stuck.
TEST_CASE("Deuterium in a document: a note keeps the note-off mode it started with", "[deuterium]")
{
  with_sampler(
      [](Rig& r) {
    r.control(QStringLiteral("Sustain"), 1.f);
    r.control(QStringLiteral("Release"), ossia::vec2f{0.05f, 0.f});
  },
      [](Rig& r) {
    // Started in Release mode, released after the switch to Ignore: stops
    r.midi(note_on(0, 60, 127));
    REQUIRE(r.tick() > 0.01);
    r.control(QStringLiteral("Note off"), true);
    r.midi(note_off(0, 60, 0));
    CHECK(r.run(0.1) < silent);

    // Started in Ignore mode, back to Release before the note-off: the note
    // still ends by its own envelope (a sustain of 1 goes straight to the
    // 0.2 s release)
    r.control(QStringLiteral("Release"), ossia::vec2f{0.2f, 0.f});
    r.midi(note_on(0, 62, 127));
    REQUIRE(r.tick() > 0.01);
    r.control(QStringLiteral("Note off"), false);
    CHECK(r.run(0.4) < silent);
  });
}
