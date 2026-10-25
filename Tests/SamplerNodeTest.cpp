// The sampler's execution node driven directly, without a score document:
// note-off modes, the MIDI channel filter, envelope times of 0 and MIDI edge
// cases, on a synthesized instrument.

// The node is private to the executor's translation unit
#include <Deuterium/GigSampler/Executor/Component.cpp>

#include <ossia/dataflow/exec_state_facade.hpp>

#include "TestHelpers.hpp"

#include <libremidi/ump_events.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Deuterium::Gig;
using Deuterium::Gig::Executor::gigsampler_node;
using Deuterium::Gig::Executor::gig_voice;

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
  explicit NodeRig(double seconds, bool oneShot = false, bool pitched = false)
  {
    st.sampleRate = rate;
    st.bufferSize = frames;
    node = std::make_shared<gigsampler_node>(st);

    auto data = std::make_shared<ossia::audio_array>(1);
    (*data)[0].resize(std::size_t(seconds * rate), 0.25f);
    GigRegion region;
    region.oneShot = oneShot;
    region.pitchTrack = pitched;
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
  //! A buffer of a paused transport (zero-length window)
  void pausedTick()
  {
    ossia::token_request tk;
    auto flicks = [](int64_t s) {
      return ossia::time_value{int64_t(s * ossia::flicks_per_second<double> / rate)};
    };
    tk.prev_date = flicks(date);
    tk.date = flicks(date);
    tk.start_sample = 0;
    tk.length_sample = 0;
    st.samples_since_start += frames;
    node->run(tk, ossia::exec_state_facade{&st});
    node->midi_in->data.messages.clear();
  }
  //! Notes of the voices still sounding (not released nor cut)
  std::vector<int> sounding() const
  {
    std::vector<int> notes;
    for(const auto& v : node->m_voices)
      if(v.playing && !v.released && !v.choked)
        notes.push_back(v.note);
    return notes;
  }
  //! The voice of the note mono / legato is sounding
  const Deuterium::Gig::Executor::gig_voice* soundingVoice() const
  {
    for(const auto& v : node->m_voices)
      if(v.playing && !v.released && !v.choked)
        return &v;
    return nullptr;
  }
  int playing() const
  {
    int n = 0;
    for(const auto& v : node->m_voices)
      n += v.playing ? 1 : 0;
    return n;
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

// Mono: a chord collapses to its last note, the others cut within the Mono
// fade; a long Mono fade lets them ring out
TEST_CASE("node: mono plays one note of a chord", "[deuterium]")
{
  NodeRig r{2., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Mono));
  r.node->set_control(MonoFade, ossia::vec2f{0.01f, 0.f});
  r.on(1, 60);
  r.on(1, 64);
  r.on(1, 67);
  r.tick();
  REQUIRE(r.sounding() == std::vector<int>{67});
  r.run(0.03);
  REQUIRE(r.playing() == 1); // the cut notes are gone after 10 ms

  NodeRig slow{2., false, true};
  slow.node->set_control(VoiceMode, int(SamplerParams::Mono));
  slow.node->set_control(MonoFade, ossia::vec2f{0.5f, 0.f});
  slow.on(1, 60);
  slow.on(1, 64);
  slow.tick();
  slow.run(0.03);
  REQUIRE(slow.playing() == 2); // still fading
  slow.run(0.6);
  REQUIRE(slow.playing() == 1);
}

// Last-note priority: releasing the sounding key returns to the most recent
// key still held; releasing another held key changes nothing
TEST_CASE("node: mono and legato return to the held note", "[deuterium]")
{
  for(int mode : {int(SamplerParams::Mono), int(SamplerParams::Legato)})
  {
    INFO("mode " << mode);
    NodeRig r{2., false, true};
    r.node->set_control(VoiceMode, mode);
    r.on(1, 60);
    r.tick();
    r.on(1, 64);
    r.tick();
    r.on(1, 67);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{67});

    // A held key that is not sounding: nothing to do
    r.off(1, 64);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{67});

    // The sounding key: back to 60, the most recent one still held
    r.off(1, 67);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{60});
    REQUIRE(r.run(0.05) > silent);

    r.off(1, 60);
    r.tick();
    REQUIRE(r.sounding().empty());
    REQUIRE(r.run(0.1) < silent);
  }
}

// Legato keeps one voice through the whole phrase: no new attack
TEST_CASE("node: legato keeps one voice", "[deuterium]")
{
  NodeRig r{2., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Legato));
  r.on(1, 60);
  r.tick();
  const auto* voice = &r.node->m_voices[0];
  for(const auto& v : r.node->m_voices)
    if(v.playing)
      voice = &v;
  r.on(1, 64);
  r.tick();
  r.off(1, 64);
  r.tick();
  REQUIRE(r.playing() == 1);
  REQUIRE(voice->playing);
  REQUIRE(voice->note == 60);
}

// Glide: Mono and Legato glide into every note, the fingered modes only into
// a note struck while another key is still held
TEST_CASE("node: fingered modes glide only into connected notes", "[deuterium]")
{
  struct Case
  {
    int mode;
    bool glidesDetached;
  };
  for(auto [mode, glidesDetached] :
      {Case{SamplerParams::Mono, true}, Case{SamplerParams::Legato, true},
       Case{SamplerParams::MonoFingered, false},
       Case{SamplerParams::LegatoFingered, false}})
  {
    INFO("mode " << mode);
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, mode);
    r.node->set_control(Glide, ossia::vec2f{1.f, 0.f});

    // Detached: 60, released, then 64
    r.on(1, 60);
    r.tick();
    r.off(1, 60);
    r.run(0.1);
    r.on(1, 64);
    r.tick();
    auto* v = r.soundingVoice();
    REQUIRE(v);
    REQUIRE(v->note == 64);
    REQUIRE(v->glide.active == glidesDetached);

    // Connected: 67 struck while 64 is held glides in every mode
    r.on(1, 67);
    r.tick();
    v = r.soundingVoice();
    REQUIRE(v);
    REQUIRE(v->note == 67);
    REQUIRE(v->glide.active);

    // Back to the held 64: connected too
    r.off(1, 67);
    r.tick();
    v = r.soundingVoice();
    REQUIRE(v);
    REQUIRE(v->note == 64);
    REQUIRE(v->glide.active);
  }
}

// Legato fingered: a detached note is a new voice with its own attack, a
// connected one keeps the voice
TEST_CASE("node: legato fingered re-articulates detached notes", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::LegatoFingered));
  r.on(1, 60);
  r.tick();
  const auto* first = r.soundingVoice();
  REQUIRE(first);
  r.on(1, 62); // connected: same voice
  r.tick();
  REQUIRE(r.soundingVoice() == first);
  r.off(1, 60);
  r.off(1, 62);
  r.tick();
  REQUIRE(r.soundingVoice() == nullptr);
  r.on(1, 64); // detached
  r.tick();
  const auto* second = r.soundingVoice();
  REQUIRE(second);
  REQUIRE(second->note == 64);
  REQUIRE(second->startOrder > first->startOrder);
}

// Constant time: an octave and a semitone both glide over the Glide time
TEST_CASE("node: the glide type control", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Legato));
  r.node->set_control(Glide, ossia::vec2f{0.1f, 0.f});
  r.node->set_control(GlideType, int(SamplerParams::GlideTime));
  REQUIRE(r.node->m_params.glideType == SamplerParams::GlideTime);
  for(int interval : {12, 1})
  {
    INFO("interval " << interval);
    r.on(1, 60);
    r.tick();
    r.on(1, 60 + interval);
    r.run(0.05);
    auto* v = r.soundingVoice();
    REQUIRE(v);
    REQUIRE(v->glide.active); // halfway
    r.run(0.07);
    REQUIRE(!v->glide.active); // over after 0.1 s
    r.off(1, 60 + interval);
    r.off(1, 60);
    r.run(0.1);
  }
}

namespace
{
std::vector<int> sorted(std::vector<int> v)
{
  std::sort(v.begin(), v.end());
  return v;
}
//! Line of the sounding voice playing `note`, -2 if none
int lineOf(const NodeRig& r, int note)
{
  for(const auto& v : r.node->m_voices)
    if(v.playing && !v.released && !v.choked && v.note == note)
      return v.line;
  return -2;
}
double pitchOf(const NodeRig& r, int note)
{
  for(const auto& v : r.node->m_voices)
    if(v.playing && !v.released && !v.choked && v.note == note)
      return v.note + (v.glide.current - v.glide.target);
  return -1000.;
}
}

// Note priority: highest / lowest keep their key against the others, and
// releasing it goes to the next one by the same rule
TEST_CASE("node: note priority", "[deuterium]")
{
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::Mono));
    r.node->set_control(NotePriority, int(SamplerParams::PriorityLowest));
    r.on(1, 60);
    r.tick();
    r.on(1, 64);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{60}); // 64 is higher: silent
    r.on(1, 55);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{55});
    r.off(1, 55);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{60});
    r.off(1, 60);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{64});
  }
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::Legato));
    r.node->set_control(NotePriority, int(SamplerParams::PriorityHighest));
    r.on(1, 64);
    r.tick();
    r.on(1, 60);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{64});
    r.on(1, 67);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{67});
    r.off(1, 67);
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{64});
  }
}

// Duophonic: the lowest and the highest held keys, each a legato line; a
// key in between changes nothing
TEST_CASE("node: duophonic", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Duophonic));
  r.on(1, 60);
  r.tick();
  REQUIRE(r.sounding() == std::vector<int>{60});
  r.on(1, 67);
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 67});
  r.on(1, 64);
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 67});
  const int high = lineOf(r, 67);
  r.on(1, 72); // the high line moves up, legato
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 72});
  REQUIRE(lineOf(r, 72) == high);
  r.off(1, 72); // back down
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 67});
  r.off(1, 60); // the low line goes to 64
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{64, 67});
  r.off(1, 67); // one key: one voice
  r.tick();
  REQUIRE(r.sounding() == std::vector<int>{64});
  r.off(1, 64);
  r.tick();
  REQUIRE(r.sounding().empty());
  REQUIRE(r.run(0.1) < silent);
}

// Line per channel: each MIDI channel is its own mono line, with its own
// pitch bend
TEST_CASE("node: one line per channel", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Mono));
  r.node->set_control(PerChannel, true);
  r.on(1, 60);
  r.on(2, 64);
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 64});
  r.on(1, 62);
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{62, 64});
  r.off(1, 62); // back to the held 60 of channel 1
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 64});
  // A note-off of another channel leaves the note alone
  r.off(3, 64);
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 64});

  // Without: one line for all channels
  r.off(1, 60);
  r.off(2, 64);
  r.run(0.1);
  r.node->set_control(PerChannel, false);
  r.on(1, 60);
  r.on(2, 64);
  r.tick();
  REQUIRE(r.sounding() == std::vector<int>{64});
}

// Poly legato, two hands: a new note continues the nearest held line within
// the max interval, a far one starts its own line
TEST_CASE("node: poly legato follows each hand", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
  r.on(1, 48);
  r.tick();
  r.on(1, 72);
  r.run(0.05);
  REQUIRE(sorted(r.sounding()) == std::vector<int>{48, 72});
  const int left = lineOf(r, 48), right = lineOf(r, 72);
  REQUIRE(left != right);

  r.on(1, 50); // left hand moves
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{50, 72});
  REQUIRE(lineOf(r, 50) == left);
  r.off(1, 48); // its old key: nothing left to release
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{50, 72});

  r.run(0.05);
  r.on(1, 71); // right hand moves
  r.tick();
  REQUIRE(lineOf(r, 71) == right);
  r.off(1, 72);
  r.run(0.05);

  r.on(1, 90); // beyond the max interval of everything: a new line
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{50, 71, 90});
  REQUIRE(lineOf(r, 90) != left);
  REQUIRE(lineOf(r, 90) != right);

  // A detached note does not connect: legato needs the key still held
  r.off(1, 50);
  r.run(0.05);
  r.on(1, 52);
  r.tick();
  REQUIRE(lineOf(r, 52) != left);
}

// Chord window: the notes of a chord wait for it, then pair with the
// previous chord in pitch order - no crossing whatever the playing order
TEST_CASE("node: poly legato pairs chords in pitch order", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
  r.node->set_control(ChordWindow, ossia::vec2f{0.03f, 0.f});
  r.on(1, 60);
  r.on(1, 64);
  r.on(1, 67);
  r.tick();
  REQUIRE(r.sounding().empty()); // waiting for the window
  r.run(0.05);
  REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 64, 67});
  const int l60 = lineOf(r, 60), l64 = lineOf(r, 64), l67 = lineOf(r, 67);

  // Next chord, rolled top-down over 20 ms, overlapping the previous one
  r.on(1, 69);
  r.tick();
  r.on(1, 65);
  r.tick();
  r.on(1, 62);
  r.run(0.05);
  r.off(1, 60);
  r.off(1, 64);
  r.off(1, 67);
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{62, 65, 69});
  CHECK(lineOf(r, 62) == l60);
  CHECK(lineOf(r, 65) == l64);
  CHECK(lineOf(r, 69) == l67);
  REQUIRE(r.playing() == 3); // moved, not stacked
}

// Poly glide: every note keeps its attack and its own voice, gliding from
// the nearest sounding note, or from the last one struck
TEST_CASE("node: poly glide sources", "[deuterium]")
{
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyGlide));
    r.node->set_control(Glide, ossia::vec2f{1.f, 0.f});
    r.on(1, 48);
    r.on(1, 60);
    r.tick();
    r.run(0.05);
    r.on(1, 62); // nearest is 60
    r.tick();
    REQUIRE(sorted(r.sounding()) == std::vector<int>{48, 60, 62});
    CHECK(std::abs(pitchOf(r, 62) - 60.) < 0.5);
    r.on(1, 46); // nearest is 48
    r.tick();
    CHECK(std::abs(pitchOf(r, 46) - 48.) < 0.5);
  }
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyGlide));
    r.node->set_control(PolyGlideFrom, int(SamplerParams::GlideFromLast));
    r.node->set_control(Glide, ossia::vec2f{1.f, 0.f});
    r.on(1, 48);
    r.tick();
    r.on(1, 60);
    r.tick();
    r.off(1, 48);
    r.off(1, 60);
    r.run(0.2);
    r.on(1, 55); // from the last struck, 60, though nothing sounds
    r.tick();
    CHECK(std::abs(pitchOf(r, 55) - 60.) < 0.5);
  }
}

namespace
{
GigRegion& rigRegion(NodeRig& r, std::size_t i = 0)
{
  return r.node->m_gigInfo->instruments[0].regions[i];
}
//! Adds a copy of the rig's region (same sample) and returns it
GigRegion& addRegion(NodeRig& r)
{
  auto& regions = r.node->m_gigInfo->instruments[0].regions;
  regions.push_back(regions[0]);
  return regions.back();
}
}

// A key no region maps leaves the lines where they are: Duophonic and Poly
// legato would otherwise cut a line and start nothing
TEST_CASE("node: unmapped keys do not cut lines", "[deuterium]")
{
  for(int mode : {int(SamplerParams::Duophonic), int(SamplerParams::PolyLegato),
                  int(SamplerParams::Legato)})
  {
    INFO("mode " << mode);
    NodeRig r{4., false, true};
    rigRegion(r).keyLow = 36;
    rigRegion(r).keyHigh = 72;
    r.node->set_control(VoiceMode, mode);
    r.node->set_control(MaxInterval, 48); // poly legato reaches key 100
    r.on(1, 60);
    r.run(0.05);
    r.on(1, 62); // second key, mapped
    r.run(0.05);
    const auto before = sorted(r.sounding());
    REQUIRE(!before.empty());
    r.on(1, 100); // unmapped, within reach of nothing / beyond the map
    r.run(0.05);
    REQUIRE(sorted(r.sounding()) == before);
    REQUIRE(r.tick() > silent);
    r.off(1, 100);
    r.tick();
    REQUIRE(sorted(r.sounding()) == before);
  }
}

// The chord window: notes released before it closes still sound (briefly)
// and end - no stuck note -; a mode change while it is open plays the chord
TEST_CASE("node: notes gathered in the chord window", "[deuterium]")
{
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.node->set_control(ChordWindow, ossia::vec2f{0.05f, 0.f});
    r.on(1, 60);
    r.tick();
    r.off(1, 60); // released while still gathered: flushed, then released
    r.tick();
    REQUIRE(r.sounding().empty());
    REQUIRE(r.run(0.2) < silent);
    REQUIRE(r.playing() == 0);
  }
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyGlide));
    r.node->set_control(ChordWindow, ossia::vec2f{0.05f, 0.f});
    r.on(1, 60);
    r.on(1, 64);
    r.tick();
    REQUIRE(r.sounding().empty());
    r.node->set_control(VoiceMode, int(SamplerParams::Poly));
    r.run(0.1);
    REQUIRE(sorted(r.sounding()) == std::vector<int>{60, 64});
  }
  {
    // The same key struck twice in the window: both strikes sound
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyGlide));
    r.node->set_control(ChordWindow, ossia::vec2f{0.05f, 0.f});
    r.on(1, 60);
    r.tick();
    r.on(1, 60);
    r.run(0.1);
    REQUIRE(r.sounding() == std::vector<int>{60, 60});
    r.off(1, 60);
    r.tick();
    REQUIRE(r.sounding().empty());
  }
  {
    // More notes than a chord holds: the extra ones are dropped, no overflow
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.node->set_control(ChordWindow, ossia::vec2f{0.05f, 0.f});
    for(int n = 0; n < 100; n++)
      r.on(1 + n / 50, 10 + n % 50);
    r.run(0.1);
    REQUIRE(r.playing() <= gigsampler_node::max_voices);
    REQUIRE(!r.sounding().empty());
  }
  {
    // A reload while a chord is gathered drops it
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.node->set_control(ChordWindow, ossia::vec2f{0.05f, 0.f});
    r.on(1, 60);
    r.tick();
    auto info = r.node->m_gigInfo;
    r.node->reload(std::move(info), rate);
    r.run(0.1);
    REQUIRE(r.playing() == 0);
  }
}

// Notes are per channel: the same key on two channels is two notes
TEST_CASE("node: note-offs match their channel", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.on(1, 60);
  r.on(2, 60);
  r.tick();
  REQUIRE(r.sounding() == std::vector<int>{60, 60});
  r.off(1, 60);
  r.tick();
  REQUIRE(r.sounding() == std::vector<int>{60});
  r.off(2, 60);
  r.tick();
  REQUIRE(r.sounding().empty());
}

// gig layer crossfades: by a controller, followed live and reset by CC 121;
// by velocity, a layer outside its ramps does not take a voice
TEST_CASE("node: layer crossfades", "[deuterium]")
{
  {
    NodeRig r{4., false, true};
    auto& g = rigRegion(r);
    g.xfadeSource = 1; // mod wheel
    g.xfadeInStart = 64;
    g.xfadeInEnd = 100;
    g.xfadeOutStart = 127;
    g.xfadeOutEnd = 127;
    r.on(1, 60);
    REQUIRE(r.run(0.05) < silent); // wheel at 0
    r.midi(libremidi::from_midi1::control_change(1, 1, 127));
    REQUIRE(r.run(0.05) > silent);
    r.midi(libremidi::from_midi1::control_change(1, 121, 0));
    REQUIRE(r.run(0.05) < silent);
  }
  {
    NodeRig r{4., false, true};
    auto& g = rigRegion(r);
    g.xfadeSource = -2; // velocity
    g.xfadeInStart = 0;
    g.xfadeInEnd = 0;
    g.xfadeOutStart = 40;
    g.xfadeOutEnd = 80;
    r.on(1, 60, 100); // past the fade-out
    r.tick();
    REQUIRE(r.playing() == 0);
    r.on(1, 62, 20);
    r.tick();
    REQUIRE(r.playing() == 1);
  }
}

// gig release triggers fade with the length of the note: a long note's
// release sample is not even started
TEST_CASE("node: release trigger decay", "[deuterium]")
{
  NodeRig r{4., false, true};
  auto& rel = addRegion(r);
  rel.releaseTrigger = true;
  rel.releaseTriggerDecay = 1.0; // silent after a second
  r.on(1, 60);
  r.run(0.1);
  r.off(1, 60);
  r.tick();
  int releases = 0;
  for(const auto& v : r.node->m_voices)
    releases += v.playing && v.region == &rel ? 1 : 0;
  REQUIRE(releases == 1);
  r.run(0.5);

  r.on(1, 62);
  r.run(1.2);
  r.off(1, 62);
  r.tick();
  releases = 0;
  for(const auto& v : r.node->m_voices)
    releases += v.playing && v.region == &rel && v.note == 62 ? 1 : 0;
  REQUIRE(releases == 0);
}

// Dynamic line ids wrap around without overflowing
TEST_CASE("node: poly line ids wrap", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
  r.node->m_nextLine = std::numeric_limits<int>::max();
  r.on(1, 40);
  r.tick();
  r.on(1, 90);
  r.tick();
  REQUIRE(lineOf(r, 40) == std::numeric_limits<int>::max());
  REQUIRE(lineOf(r, 90) == 64); // back to the first dynamic line
}

// Poly glide from the last note: not further than the max interval
TEST_CASE("node: poly glide respects the max interval", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::PolyGlide));
  r.node->set_control(PolyGlideFrom, int(SamplerParams::GlideFromLast));
  r.node->set_control(MaxInterval, 5);
  r.node->set_control(Glide, ossia::vec2f{1.f, 0.f});
  r.on(1, 60);
  r.tick();
  r.on(1, 72); // 12 away: no glide
  r.tick();
  CHECK(std::abs(pitchOf(r, 72) - 72.) < 1e-9);
  r.on(1, 75); // 3 away from the last, 72
  r.tick();
  CHECK(pitchOf(r, 75) < 74.);
}

// Voices stolen at the polyphony cap leave the lines consistent
TEST_CASE("node: line voices stolen at the cap", "[deuterium]")
{
  for(int mode : {int(SamplerParams::PolyLegato), int(SamplerParams::Duophonic),
                  int(SamplerParams::Legato)})
  {
    INFO("mode " << mode);
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, mode);
    r.node->set_control(Polyphony, 1);
    for(int n = 40; n < 80; n += 3)
    {
      r.on(1, n);
      r.tick();
    }
    REQUIRE(r.playing() <= 1);
    for(int n = 40; n < 80; n += 3)
    {
      r.off(1, n);
      r.tick();
    }
    REQUIRE(r.run(0.2) < silent);
    REQUIRE(r.playing() == 0);
  }
}

// Duophonic with a line pair per channel
TEST_CASE("node: duophonic per channel", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Duophonic));
  r.node->set_control(PerChannel, true);
  r.on(1, 60);
  r.on(1, 72);
  r.on(2, 40);
  r.on(2, 50);
  r.on(2, 45); // in between on channel 2: nothing
  r.tick();
  REQUIRE(sorted(r.sounding()) == std::vector<int>{40, 50, 60, 72});
}

// Channel pressure drives the layer crossfades bound to it
TEST_CASE("node: channel pressure crossfades", "[deuterium]")
{
  NodeRig r{4., false, true};
  auto& g = rigRegion(r);
  g.xfadeSource = 128; // channel pressure
  g.xfadeInStart = 64;
  g.xfadeInEnd = 100;
  g.xfadeOutStart = 127;
  g.xfadeOutEnd = 127;
  r.on(1, 60);
  REQUIRE(r.run(0.05) < silent);
  r.midi(libremidi::from_midi1::aftertouch(1, 127));
  REQUIRE(r.run(0.05) > silent);
  r.midi(libremidi::from_midi1::aftertouch(1, 0));
  REQUIRE(r.run(0.05) < silent);
}

// Poly legato: the old key lifted in the same buffer as the new note, or
// while the chord window is open, is normal legato playing - the line moves
TEST_CASE("node: poly legato when the old key lifts early", "[deuterium]")
{
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.on(1, 60);
    r.run(0.05);
    const int line = lineOf(r, 60);
    r.on(1, 62);
    r.off(1, 60); // same buffer
    r.tick();
    REQUIRE(r.sounding() == std::vector<int>{62});
    CHECK(lineOf(r, 62) == line);
    CHECK(r.playing() == 1);
  }
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.node->set_control(ChordWindow, ossia::vec2f{0.03f, 0.f});
    r.on(1, 60);
    r.on(1, 64);
    r.run(0.05);
    const int l60 = lineOf(r, 60), l64 = lineOf(r, 64);
    r.on(1, 62);
    r.on(1, 65);
    r.tick();
    r.off(1, 60); // lifted inside the window
    r.off(1, 64);
    r.run(0.05);
    REQUIRE(sorted(r.sounding()) == std::vector<int>{62, 65});
    CHECK(lineOf(r, 62) == l60);
    CHECK(lineOf(r, 65) == l64);
    CHECK(r.playing() == 2);
  }
  {
    // A lifted line nothing takes over is released once the chord is paired
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.node->set_control(ChordWindow, ossia::vec2f{0.03f, 0.f});
    r.on(1, 40);
    r.run(0.05);
    r.on(1, 80); // far from 40: a new line
    r.tick();
    r.off(1, 40);
    r.run(0.05);
    REQUIRE(r.sounding() == std::vector<int>{80});
    r.off(1, 80);
    REQUIRE(r.run(0.2) < silent);
  }
}

// Release samples belong to keys that sounded: not to a key the note
// priority kept silent, nor to a duophonic inner key
TEST_CASE("node: release triggers of keys that never sounded", "[deuterium]")
{
  auto releases = [](NodeRig& r, const GigRegion& rel, int note) {
    int n = 0;
    for(const auto& v : r.node->m_voices)
      n += v.playing && v.region == &rel && v.note == note ? 1 : 0;
    return n;
  };
  {
    NodeRig r{4., false, true};
    auto& rel = addRegion(r);
    rel.releaseTrigger = true;
    r.node->set_control(VoiceMode, int(SamplerParams::Mono));
    r.node->set_control(NotePriority, int(SamplerParams::PriorityHighest));
    r.on(1, 72);
    r.tick();
    r.on(1, 60); // never sounds
    r.tick();
    r.off(1, 60);
    r.tick();
    CHECK(releases(r, rel, 60) == 0);
    r.off(1, 72);
    r.tick();
    CHECK(releases(r, rel, 72) == 1);
  }
  {
    NodeRig r{4., false, true};
    auto& rel = addRegion(r);
    rel.releaseTrigger = true;
    r.node->set_control(VoiceMode, int(SamplerParams::Duophonic));
    r.on(1, 48);
    r.on(1, 72);
    r.tick();
    r.on(1, 60); // inner key
    r.tick();
    r.off(1, 60);
    r.tick();
    CHECK(releases(r, rel, 60) == 0);
  }
  {
    // Poly: any struck key fires its release samples, as before
    NodeRig r{4., false, true};
    auto& rel = addRegion(r);
    rel.releaseTrigger = true;
    r.on(1, 60);
    r.tick();
    r.off(1, 60);
    r.tick();
    CHECK(releases(r, rel, 60) == 1);
  }
}

// Line per channel: each channel's pitch bend moves its own voices; CC 121
// resets the bend of its channel only
TEST_CASE("node: per-channel pitch bend", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Mono));
  r.node->set_control(PerChannel, true);
  r.node->set_control(BendRange, 12.f);
  r.on(1, 60);
  r.on(2, 60);
  // channel 1 up an octave (the helper takes a 16-bit value)
  r.midi(libremidi::from_midi1::pitch_bend(1, 65535));
  r.run(0.1);
  const gig_voice *v1 = nullptr, *v2 = nullptr;
  for(const auto& v : r.node->m_voices)
    if(v.playing && !v.released)
      (v.channel == 1 ? v1 : v2) = &v; // from_midi1 channels are 0-based
  REQUIRE(v1);
  REQUIRE(v2);
  // The bent voice reads its sample about twice as fast
  CHECK(v1->head.pos > 1.9 * v2->head.pos);

  r.midi(libremidi::from_midi1::pitch_bend(2, 0));
  r.midi(libremidi::from_midi1::control_change(1, 121, 0));
  r.tick();
  CHECK(r.node->m_bendCh[1] == 0.);
  CHECK(r.node->m_bendCh[2] < -0.9);
}

// The gig filter at note-on: type from the file, cutoff scaled by the
// velocity table and the key tracking; EG2 closes it after the attack
TEST_CASE("node: gig filter", "[deuterium]")
{
  NodeRig r{4., false, true};
  auto& g = rigRegion(r);
  g.vcfEnabled = true;
  g.vcfType = SamplerParams::FilterHighpass;
  g.vcfCutoff = 100;
  auto table = std::make_shared<std::array<float, 128>>();
  table->fill(0.5f);
  g.vcfVelTable = table;
  g.vcfKeytrackBreakpoint = 60;
  g.gigCutoffEnv = true;
  g.eg2Attack = 0.f;
  g.eg2Decay = 0.05f;
  g.eg2Sustain = 0.f;
  g.eg2Release = 0.05f;
  r.on(1, 72);
  r.tick();
  const gig_voice* v = nullptr;
  for(const auto& x : r.node->m_voices)
    if(x.playing)
      v = &x;
  REQUIRE(v);
  CHECK(v->filterOn);
  CHECK(v->filterTypeEff == SamplerParams::FilterHighpass);
  // 100 * 0.5 * 2^(12/12) = 100 units
  CHECK(std::abs(v->cutoffBaseHz - gigCutoffToHz(100.)) < 1e-6);
  CHECK(std::abs(v->gigFcSpanCents - 100. * gigCentsPerCutoffUnit) < 1e-6);
  r.run(0.1);
  CHECK(v->fileModEnv.value < 0.01); // decayed to its zero sustain
}

// gig EG1 decay 2 and pre-attack, with the file's envelope; the knobs'
// envelope ignores them
TEST_CASE("node: gig decay 2 and pre-attack", "[deuterium]")
{
  {
    NodeRig r{4., false, true};
    rigRegion(r).eg1Decay2 = 0.1;
    rigRegion(r).eg1Sustain = 1.0;
    rigRegion(r).eg1Attack = 0.;
    r.node->set_control(EnvFromFile, true);
    r.on(1, 60);
    r.run(0.3);
    CHECK(r.playing() == 0); // faded out while held
  }
  {
    NodeRig r{4., false, true};
    rigRegion(r).eg1Decay2 = 0.1;
    r.on(1, 60); // the rig's knob envelope: sustain 1
    REQUIRE(r.run(0.3) > silent);
  }
  {
    NodeRig r{4., false, true};
    rigRegion(r).eg1PreAttack = 1.0;
    rigRegion(r).eg1Attack = 1.0;
    r.node->set_control(EnvFromFile, true);
    r.on(1, 60);
    const double first = r.tick();
    // Full level from the start (0.25 at -3 dB centre pan), not the first
    // ms of a 1 s swell
    CHECK(first > 0.15);
  }
}

// Switching mode, or the line per channel, with keys held leaves no stuck
// note once they are released
TEST_CASE("node: mode switches with keys held", "[deuterium]")
{
  const int modes[]
      = {SamplerParams::Poly,        SamplerParams::Mono,     SamplerParams::Legato,
         SamplerParams::MonoFingered, SamplerParams::LegatoFingered,
         SamplerParams::Duophonic,   SamplerParams::PolyGlide, SamplerParams::PolyLegato};
  for(int from : modes)
    for(int to : modes)
    {
      INFO("from " << from << " to " << to);
      NodeRig r{4., false, true};
      r.node->set_control(VoiceMode, from);
      r.on(1, 60);
      r.on(1, 64);
      r.tick();
      r.on(1, 67);
      r.tick();
      r.node->set_control(VoiceMode, to);
      r.node->set_control(PerChannel, true);
      r.on(1, 71);
      r.tick();
      r.off(1, 60);
      r.off(1, 64);
      r.tick();
      r.off(1, 67);
      r.off(1, 71);
      r.tick();
      REQUIRE(r.run(0.2) < silent);
      REQUIRE(r.playing() == 0);
    }
}

// Note-offs ignored: the lines end by their envelope, nothing piles up
TEST_CASE("node: lines with note-offs ignored", "[deuterium]")
{
  for(int mode : {int(SamplerParams::Legato), int(SamplerParams::Duophonic),
                  int(SamplerParams::PolyLegato)})
  {
    INFO("mode " << mode);
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, mode);
    r.node->set_control(NoteOff, true);
    for(int n = 50; n < 70; n += 2)
    {
      r.on(1, n);
      r.tick();
      r.off(1, n);
      r.tick();
    }
    REQUIRE(r.playing() <= gigsampler_node::max_voices);
    // The rig's envelope: decay 0.1 s, then the release of an ignored note-off
    REQUIRE(r.run(0.5) < silent);
  }
}

// A chord gathered while the transport is paused waits for it to run; a
// MIDI channel filter change drops it
TEST_CASE("node: gathered chord and transport / channel changes", "[deuterium]")
{
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.node->set_control(ChordWindow, ossia::vec2f{0.02f, 0.f});
    r.on(1, 60);
    r.tick();
    for(int i = 0; i < 10; i++)
      r.pausedTick();
    REQUIRE(r.playing() == 0);
    r.run(0.05);
    REQUIRE(r.sounding() == std::vector<int>{60});
  }
  {
    NodeRig r{4., false, true};
    r.node->set_control(VoiceMode, int(SamplerParams::PolyLegato));
    r.node->set_control(ChordWindow, ossia::vec2f{0.05f, 0.f});
    r.on(1, 60);
    r.tick();
    r.node->set_control(MidiChannel, 3);
    r.run(0.1);
    REQUIRE(r.playing() == 0);
  }
}

// Chromatic mode: a legato step plays the key's own pitch
TEST_CASE("node: chromatic legato", "[deuterium]")
{
  NodeRig r{4., false, true};
  r.node->set_control(VoiceMode, int(SamplerParams::Legato));
  r.node->set_control(Chromatic, true);
  r.node->set_control(ChromaticRoot, 60);
  r.on(1, 64);
  r.tick();
  r.on(1, 67);
  r.tick();
  REQUIRE(r.sounding() == std::vector<int>{67});
  CHECK(std::abs(pitchOf(r, 67) - 67.) < 1e-9);
  CHECK(r.playing() == 1);
}
