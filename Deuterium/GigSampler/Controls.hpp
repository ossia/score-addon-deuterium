#pragma once
#include <Process/Dataflow/WidgetInlets.hpp>

#include <Deuterium/GigSampler/SamplerEngine.hpp>

#include <vector>

namespace Deuterium::Gig
{
// The global sampler controls. The enum order defines the process inlet
// order (right after the MIDI inlet) and is what the executor uses to route
// values into SamplerParams — keep the three in sync.
enum SamplerControl : int
{
  Volume,
  Pan,
  Transpose,
  FineTune,
  BendRange,

  Attack,
  Decay,
  Sustain,
  Release,

  FilterType,
  Cutoff,
  Resonance,
  FilterKeytrack,
  FilterEnvAmount,
  FilterEnvAttack,
  FilterEnvDecay,
  FilterEnvSustain,
  FilterEnvRelease,
  VelToCutoff,

  VelAmount,
  VelCurve,
  VelToStart,
  VelXfade,

  VoiceMode,
  Glide,
  Polyphony,

  StartOffset,
  Reverse,
  LoopMode,
  LoopXfade,

  Lofi,
  PitchEnvAmount,
  PitchEnvDecay,

  LfoDest,
  LfoRate,
  LfoDepth,
  LfoDelay,

  RoundRobin,

  Chromatic,
  ChromaticRoot,

  // Appended late (order is the serialization contract; ensureControls
  // appends missing trailing controls when loading older documents)
  EnvFromFile,
  Instrument,
  File,
  VelToPitchEnv,

  ControlCount
};

// Controls carrying a Process::TimeChooser. Their value is vec2f{x, sync}:
// x is seconds when sync < 0.5, a fraction of a whole note otherwise —
// resolved against the current tempo on the execution side. They also accept
// a plain float (legacy documents, graph modulation), interpreted with the
// control's historical unit: seconds, except LfoRate where a float is Hz.
inline constexpr bool isTimeControl(int c) noexcept
{
  switch(c)
  {
    case Attack:
    case Decay:
    case Release:
    case FilterEnvAttack:
    case FilterEnvDecay:
    case FilterEnvRelease:
    case Glide:
    case LfoRate:
    case LfoDelay:
      return true;
    default:
      return false;
  }
}

//! Musical position x (fraction of a whole note) at the given tempo, in
//! seconds. Matches the interpretation used by score's other time choosers.
inline constexpr float syncTimeToSeconds(float x, double tempo) noexcept
{
  const float q_ratio = 4.f * x; // 1 == a whole note, 0.25 == a quarter
  const float beat_dur = float(60. / tempo);
  return q_ratio * beat_dur;
}

inline std::vector<Process::ControlInlet*> makeSamplerControls(QObject* parent)
{
  std::vector<Process::ControlInlet*> v(ControlCount);
  const auto id = [](int c) { return Id<Process::Port>(1 + c); };
  const auto flt = [&](int c, float min, float max, float init, const QString& name) {
    v[c] = new Process::FloatSlider{min, max, init, name, id(c), parent};
  };
  const auto time = [&](int c, float min, float max, float init, const QString& name) {
    auto* tc = new Process::TimeChooser{min, max, init, name, id(c), parent};
    // The stock TimeChooser defaults to tempo-synced; these controls default
    // to their historical free-running time in seconds.
    tc->setValue(ossia::vec2f{init, 0.f});
    tc->setInit(tc->value());
    v[c] = tc;
  };
  const auto combo
      = [&](int c, std::vector<std::pair<QString, ossia::value>> alts, int init,
            const QString& name) {
          v[c] = new Process::ComboBox{std::move(alts), init, name, id(c), parent};
        };

  flt(Volume, 0.f, 2.f, 1.f, QStringLiteral("Volume"));
  flt(Pan, -1.f, 1.f, 0.f, QStringLiteral("Pan"));
  flt(Transpose, -24.f, 24.f, 0.f, QStringLiteral("Transpose"));
  flt(FineTune, -100.f, 100.f, 0.f, QStringLiteral("Fine tune"));
  flt(BendRange, 0.f, 24.f, 2.f, QStringLiteral("Bend range"));

  // Only in effect when EnvFromFile is off; historical documents instead
  // carry sliders whose negative values mean "use the file's envelope".
  time(Attack, 0.001f, 5.f, 0.001f, QStringLiteral("Attack"));
  time(Decay, 0.001f, 5.f, 0.15f, QStringLiteral("Decay"));
  flt(Sustain, 0.f, 1.f, 1.f, QStringLiteral("Sustain"));
  time(Release, 0.001f, 8.f, 0.05f, QStringLiteral("Release"));

  combo(
      FilterType,
      {{QStringLiteral("From file"), 0},
       {QStringLiteral("Off"), 1},
       {QStringLiteral("Lowpass"), 2},
       {QStringLiteral("Highpass"), 3},
       {QStringLiteral("Bandpass"), 4},
       {QStringLiteral("Notch"), 5}},
      0, QStringLiteral("Filter"));
  v[Cutoff] = new Process::LogFloatSlider{
      20.f, 20000.f, 18000.f, QStringLiteral("Cutoff"), id(Cutoff), parent};
  flt(Resonance, 0.f, 1.f, 0.f, QStringLiteral("Resonance"));
  flt(FilterKeytrack, 0.f, 1.f, 0.f, QStringLiteral("Filter keytrack"));
  flt(FilterEnvAmount, -1.f, 1.f, 0.f, QStringLiteral("Filter env"));
  time(FilterEnvAttack, 0.001f, 5.f, 0.001f, QStringLiteral("Filter env attack"));
  time(FilterEnvDecay, 0.001f, 5.f, 0.15f, QStringLiteral("Filter env decay"));
  flt(FilterEnvSustain, 0.f, 1.f, 0.f, QStringLiteral("Filter env sustain"));
  time(FilterEnvRelease, 0.001f, 8.f, 0.05f, QStringLiteral("Filter env release"));
  flt(VelToCutoff, 0.f, 1.f, 0.f, QStringLiteral("Vel > cutoff"));

  flt(VelAmount, 0.f, 1.f, 1.f, QStringLiteral("Vel > volume"));
  // "Hard" (v^2) is exactly the SF2 default velocity-to-attenuation curve
  // (concave, 960 cB): the default every mainstream SoundFont player uses
  combo(
      VelCurve,
      {{QStringLiteral("Linear"), 0},
       {QStringLiteral("Soft"), 1},
       {QStringLiteral("Hard"), 2}},
      2, QStringLiteral("Vel curve"));
  flt(VelToStart, 0.f, 1.f, 0.f, QStringLiteral("Vel > start"));
  flt(VelXfade, 0.f, 1.f, 0.f, QStringLiteral("Layer crossfade"));

  combo(
      VoiceMode,
      {{QStringLiteral("Poly"), 0},
       {QStringLiteral("Mono"), 1},
       {QStringLiteral("Legato"), 2}},
      0, QStringLiteral("Voices"));
  time(Glide, 0.f, 2.f, 0.f, QStringLiteral("Glide"));
  v[Polyphony] = new Process::IntSlider{
      1, 64, 64, QStringLiteral("Polyphony"), id(Polyphony), parent};

  flt(StartOffset, 0.f, 1.f, 0.f, QStringLiteral("Start"));
  v[Reverse]
      = new Process::Toggle{false, QStringLiteral("Reverse"), id(Reverse), parent};
  combo(
      LoopMode,
      {{QStringLiteral("From file"), 0},
       {QStringLiteral("Off"), 1},
       {QStringLiteral("Forward"), 2},
       {QStringLiteral("Ping-pong"), 3},
       {QStringLiteral("Until release"), 4}},
      0, QStringLiteral("Loop"));
  flt(LoopXfade, 0.f, 0.5f, 0.f, QStringLiteral("Loop crossfade"));

  flt(Lofi, 0.f, 1.f, 0.f, QStringLiteral("Lo-fi"));
  flt(PitchEnvAmount, -24.f, 24.f, 0.f, QStringLiteral("Pitch env"));
  flt(PitchEnvDecay, 0.001f, 2.f, 0.08f, QStringLiteral("Pitch env decay"));
  flt(VelToPitchEnv, -1.f, 1.f, 0.f, QStringLiteral("Vel > pitch env"));

  combo(
      LfoDest,
      {{QStringLiteral("Off"), 0},
       {QStringLiteral("Pitch"), 1},
       {QStringLiteral("Cutoff"), 2},
       {QStringLiteral("Volume"), 3},
       {QStringLiteral("Pan"), 4}},
      0, QStringLiteral("LFO"));
  // Stored as a period so that it can be tempo-synced; a plain float on
  // the inlet still means Hz (legacy documents).
  time(LfoRate, 0.025f, 20.f, 0.2f, QStringLiteral("LFO period"));
  flt(LfoDepth, 0.f, 1.f, 0.f, QStringLiteral("LFO depth"));
  time(LfoDelay, 0.f, 5.f, 0.f, QStringLiteral("LFO delay"));

  combo(
      RoundRobin,
      {{QStringLiteral("From file"), 0},
       {QStringLiteral("Off"), 1},
       {QStringLiteral("Cycle"), 2},
       {QStringLiteral("Random"), 3}},
      0, QStringLiteral("Alternate"));

  v[Chromatic] = new Process::Toggle{
      false, QStringLiteral("Chromatic"), id(Chromatic), parent};
  v[ChromaticRoot] = new Process::IntSlider{
      0, 127, 60, QStringLiteral("Chromatic root"), id(ChromaticRoot), parent};

  // The underlying value stays a bool so the execution side and historical
  // documents are unaffected by the switch from a toggle to a selector.
  v[EnvFromFile] = new Process::ComboBox{
      {{QStringLiteral("From file"), true}, {QStringLiteral("Custom"), false}}, true,
      QStringLiteral("Envelope source"), id(EnvFromFile), parent};

  // Which instrument of the loaded file plays; switching reloads only the
  // sample data, every other control keeps its value.
  v[Instrument] = new Process::IntSlider{
      0, 127, 0, QStringLiteral("Instrument"), id(Instrument), parent};

  v[File] = new Process::FileChooser{
      QString{},
      QStringLiteral("Sample banks (*.gig *.dls *.sf2 *.kmp *.xml *.wav *.flac "
                     "*.ogg *.aiff *.aif *.mp3)"),
      QStringLiteral("File"), id(File), parent};

  return v;
}

// Applies one control's ossia value onto the parameter block; index is a
// SamplerControl. Invalid values are ignored field-by-field.
void applySamplerControl(SamplerParams& p, int control, const ossia::value& val);
}
