// The sampler's execution node driven directly, without a score document:
// note-off modes, the MIDI channel filter, envelope times of 0 and MIDI edge
// cases, on a synthesized instrument.

// The node is private to the executor's translation unit
#include <Deuterium/GigSampler/Executor/Component.cpp>

#include <ossia/dataflow/exec_state_facade.hpp>

#include "TestHelpers.hpp"

#include <libremidi/ump_events.hpp>

#include <cmath>

using namespace Deuterium::Gig;
using Deuterium::Gig::Executor::gigsampler_node;

namespace
{
constexpr double rate = 48000.;
constexpr int frames = 480; // 10 ms

struct NodeRig
{
  ossia::execution_state st;
  std::shared_ptr<gigsampler_node> node;
  int64_t date{};

  //! One region over the whole keyboard playing `seconds` of a constant
  //! level: voices add up exactly, whatever their start.
  explicit NodeRig(double seconds, bool oneShot = false)
  {
    st.sampleRate = rate;
    st.bufferSize = frames;
    node = std::make_shared<gigsampler_node>(st);

    auto data = std::make_shared<ossia::audio_array>(1);
    (*data)[0].resize(std::size_t(seconds * rate), 0.25f);
    GigRegion region;
    region.oneShot = oneShot;
    region.pitchTrack = false;
    region.sample.sampleRate = uint32_t(rate);
    region.sample.data = std::move(data);
    GigInstrument instrument;
    instrument.regions.push_back(std::move(region));
    auto info = std::make_shared<GigFileInfo>();
    info->instruments.push_back(std::move(instrument));
    node->reload(std::move(info), rate);

    // The knobs' envelope, with a short release: a released note is quickly
    // silent
    node->set_control(EnvFromFile, false);
    node->set_control(Attack, ossia::vec2f{0.001f, 0.f});
    node->set_control(Decay, ossia::vec2f{0.1f, 0.f});
    node->set_control(Sustain, 1.f);
    node->set_control(Release, ossia::vec2f{0.02f, 0.f});
  }

  void midi(const libremidi::ump& m) { node->midi_in->data.messages.push_back(m); }
  void on(int channel, int note, int vel = 127)
  {
    midi(libremidi::from_midi1::note_on(channel, note, vel));
  }
  void off(int channel, int note) { midi(libremidi::from_midi1::note_off(channel, note, 0)); }

  //! Runs one buffer; the RMS of the left channel.
  double tick()
  {
    ossia::token_request tk;
    auto flicks = [](int64_t s) {
      return ossia::time_value{int64_t(s * ossia::flicks_per_second<double> / rate)};
    };
    tk.prev_date = flicks(date);
    tk.date = flicks(date + frames);
    tk.start_sample = 0;
    tk.length_sample = frames;
    date += frames;
    st.samples_since_start += frames;
    node->run(tk, ossia::exec_state_facade{&st});
    node->midi_in->data.messages.clear();

    auto& out = node->audio_out->data;
    double sum = 0.;
    if(out.channels() > 0)
    {
      auto& ch = out.channel(0);
      for(int i = 0; i < frames && i < int(ch.size()); i++)
      {
        REQUIRE(std::isfinite(ch[i]));
        sum += ch[i] * ch[i];
      }
      for(auto& c : out.get())
        std::fill(c.begin(), c.end(), 0.);
    }
    return std::sqrt(sum / frames);
  }
  double run(double seconds)
  {
    double v = 0.;
    for(int i = 0; i < int(seconds * rate / frames); i++)
      v = tick();
    return v;
  }
};

constexpr double silent = 1e-4;
}

TEST_CASE("node: a note-off releases the note", "[deuterium]")
{
  NodeRig rig{2.};
  rig.on(0, 60);
  REQUIRE(rig.tick() > 0.05);
  rig.off(0, 60);
  CHECK(rig.run(0.1) < silent);
}

// Ignore turns the note into attack, decay and release: a sustain of 1 goes
// on to the release right after the attack.
TEST_CASE("node: ignored note-offs let the envelope end the note", "[deuterium]")
{
  NodeRig rig{2.};
  rig.node->set_control(NoteOff, true);
  rig.node->set_control(Release, ossia::vec2f{0.5f, 0.f});
  rig.on(0, 60);
  REQUIRE(rig.tick() > 0.05);
  rig.off(0, 60);
  CHECK(rig.run(0.1) > 0.01); // the note-off did nothing
  CHECK(rig.run(0.6) < silent); // the release ended it
}

// A one-shot (a drum of a Hydrogen kit) plays its whole sample in both modes:
// Ignore must not cut it after its attack.
TEST_CASE("node: one-shots play their sample to the end with note-offs ignored", "[deuterium]")
{
  for(bool ignore : {false, true})
  {
    INFO("ignore " << ignore);
    NodeRig rig{0.5, true};
    rig.node->set_control(NoteOff, ignore);
    rig.on(0, 36);
    REQUIRE(rig.tick() > 0.05);
    rig.off(0, 36);
    CHECK(rig.run(0.3) > 0.05);
    CHECK(rig.run(0.3) < silent);
  }
}

// Struck again, a one-shot overlaps its previous hit as in the Release mode
TEST_CASE("node: one-shots struck again overlap with note-offs ignored", "[deuterium]")
{
  NodeRig rig{2., true};
  rig.node->set_control(NoteOff, true);
  rig.node->set_control(Sustain, 0.5f);
  rig.node->set_control(Decay, ossia::vec2f{0.01f, 0.f});
  rig.on(0, 60);
  const double single = rig.run(0.05);
  rig.on(0, 60);
  CHECK(rig.run(0.2) / single > 1.8);
}

TEST_CASE("node: a note held when note-offs become ignored still stops", "[deuterium]")
{
  NodeRig rig{2.};
  rig.on(0, 60);
  REQUIRE(rig.tick() > 0.05);
  rig.node->set_control(NoteOff, true);
  rig.off(0, 60);
  CHECK(rig.run(0.1) < silent);
}

TEST_CASE("node: the MIDI channel filter", "[deuterium]")
{
  NodeRig rig{2.};
  rig.node->set_control(MidiChannel, 2);
  rig.on(0, 60); // channel 1 on the wire is 0
  CHECK(rig.tick() < silent);
  rig.on(1, 62);
  CHECK(rig.tick() > 0.05);
  rig.off(0, 62); // another channel's note-off
  CHECK(rig.run(0.1) > 0.05);
  rig.off(1, 62);
  CHECK(rig.run(0.1) < silent);

  // Out of range values (graph modulation) are clamped
  rig.node->set_control(MidiChannel, 42);
  CHECK(rig.node->m_params.midiChannel == 16);
  rig.node->set_control(MidiChannel, -3);
  CHECK(rig.node->m_params.midiChannel == 0);
}

// The note-offs of what plays would now be filtered out: a stuck note unless
// the switch releases it.
TEST_CASE("node: changing the MIDI channel mid-note leaves no stuck note", "[deuterium]")
{
  NodeRig rig{4.};
  rig.on(1, 60);
  REQUIRE(rig.tick() > 0.05);
  rig.node->set_control(MidiChannel, 1);
  rig.off(1, 60);
  CHECK(rig.run(0.1) < silent);

  // Widening the filter to every channel keeps the notes playing
  rig.on(0, 62);
  REQUIRE(rig.tick() > 0.05);
  rig.node->set_control(MidiChannel, 0);
  CHECK(rig.run(0.1) > 0.05);
  rig.off(0, 62);
  CHECK(rig.run(0.1) < silent);
}

TEST_CASE("node: MIDI edge cases", "[deuterium]")
{
  NodeRig rig{4.};

  // A note-off with no note-on, and a note-off before its note-on in the
  // same buffer
  rig.off(0, 60);
  CHECK(rig.tick() < silent);
  rig.off(0, 61);
  rig.on(0, 61);
  CHECK(rig.tick() > 0.05);
  rig.off(0, 61);
  CHECK(rig.run(0.1) < silent);

  // Every key at once, with note-offs ignored: voices are stolen, and All
  // Notes Off still ends notes that ignore their note-off
  rig.node->set_control(NoteOff, true);
  rig.node->set_control(Sustain, 0.5f);
  rig.node->set_control(Decay, ossia::vec2f{2.f, 0.f});
  for(int note = 0; note < 128; note++)
    rig.on(0, note);
  CHECK(rig.tick() > 0.05);
  rig.midi(libremidi::from_midi1::control_change(0, 123, 0));
  CHECK(rig.run(0.1) < silent);

  // A black-MIDI rate: thousands of events in one buffer
  for(int i = 0; i < 4000; i++)
  {
    rig.on(0, i % 128, 1 + i % 127);
    rig.off(0, (i + 64) % 128);
  }
  rig.tick();
  rig.midi(libremidi::from_midi1::control_change(0, 120, 0)); // All Sound Off
  rig.tick();
  CHECK(rig.tick() < silent);
}

// Every envelope time of 0: no division by zero, no click-free guarantee
// lost (the engine keeps a minimal ramp), no NaN.
TEST_CASE("node: envelope times of 0", "[deuterium]")
{
  NodeRig rig{2.};
  for(int c : {Attack, Decay, Release, FilterEnvAttack, FilterEnvDecay, FilterEnvRelease,
               PitchEnvDecay})
    rig.node->set_control(c, ossia::vec2f{0.f, 0.f});
  rig.node->set_control(Sustain, 0.5f);
  rig.node->set_control(PitchEnvAmount, 12.f);
  rig.node->set_control(FilterType, int(SamplerParams::FilterLowpass));
  rig.node->set_control(FilterEnvAmount, 1.f);

  rig.on(0, 60);
  CHECK(rig.tick() > 0.05);
  rig.off(0, 60);
  CHECK(rig.run(0.05) < silent);
}

// The pitch envelope decay became a time chooser: {x, mode} values, the
// legacy plain float of older documents (seconds), and a tempo-synced value.
TEST_CASE("node: pitch envelope decay values", "[deuterium]")
{
  NodeRig rig{1.};
  rig.node->set_control(PitchEnvDecay, 0.3f);
  CHECK(approxEq(rig.node->m_params.pitchEnvDecay, 0.3f));
  rig.node->set_control(PitchEnvDecay, ossia::vec2f{0.4f, 0.f});
  CHECK(approxEq(rig.node->m_params.pitchEnvDecay, 0.4f));
  // A quarter note at the default tempo of 120
  rig.node->set_control(PitchEnvDecay, ossia::vec2f{0.25f, 1.f});
  CHECK(approxEq(rig.node->m_params.pitchEnvDecay, 0.5f));
}

// Ignore must not turn a note into a one-shot, which does not loop: a sampled
// piano is a short attack and a loop, faded out by its envelope.
TEST_CASE("node: ignored note-offs keep the sample's loop", "[deuterium]")
{
  NodeRig rig{0.1};
  rig.node->set_control(LoopMode, int(SamplerParams::LoopForward));
  rig.node->set_control(NoteOff, true);
  rig.node->set_control(Sustain, 0.f);
  rig.node->set_control(Decay, ossia::vec2f{1.f, 0.f});
  rig.on(0, 60);
  REQUIRE(rig.tick() > 0.05);
  rig.off(0, 60);
  // Well past the end of the 0.1 s sample, the loop still sounds
  CHECK(rig.run(0.3) > 1e-3);
  // and the decay ends it
  CHECK(rig.run(1.2) < silent);
}

// A note keeps the mode it started in: started with note-offs ignored, it
// still ends by its own envelope when the mode goes back to Release.
TEST_CASE("node: a note started with note-offs ignored ends by its envelope", "[deuterium]")
{
  NodeRig rig{2.};
  rig.node->set_control(NoteOff, true);
  rig.node->set_control(Release, ossia::vec2f{0.2f, 0.f});
  rig.on(0, 60);
  REQUIRE(rig.tick() > 0.05);
  rig.node->set_control(NoteOff, false);
  rig.off(0, 60);
  CHECK(rig.run(0.5) < silent);
}
