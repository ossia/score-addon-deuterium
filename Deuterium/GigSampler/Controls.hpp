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

  ControlCount
};

inline std::vector<Process::ControlInlet*> makeSamplerControls(QObject* parent)
{
  std::vector<Process::ControlInlet*> v(ControlCount);
  const auto id = [](int c) { return Id<Process::Port>(1 + c); };
  const auto flt = [&](int c, float min, float max, float init, const QString& name) {
    v[c] = new Process::FloatSlider{min, max, init, name, id(c), parent};
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

  // < 0 means "use the value from the file"
  flt(Attack, -0.001f, 5.f, -0.001f, QStringLiteral("Attack"));
  flt(Decay, -0.001f, 5.f, -0.001f, QStringLiteral("Decay"));
  flt(Sustain, -0.01f, 1.f, -0.01f, QStringLiteral("Sustain"));
  flt(Release, -0.001f, 8.f, -0.001f, QStringLiteral("Release"));

  combo(
      FilterType,
      {{QStringLiteral("From file"), 0},
       {QStringLiteral("Off"), 1},
       {QStringLiteral("Lowpass"), 2},
       {QStringLiteral("Highpass"), 3},
       {QStringLiteral("Bandpass"), 4},
       {QStringLiteral("Notch"), 5}},
      0, QStringLiteral("Filter"));
  flt(Cutoff, 20.f, 20000.f, 18000.f, QStringLiteral("Cutoff"));
  flt(Resonance, 0.f, 1.f, 0.f, QStringLiteral("Resonance"));
  flt(FilterKeytrack, 0.f, 1.f, 0.f, QStringLiteral("Filter keytrack"));
  flt(FilterEnvAmount, -1.f, 1.f, 0.f, QStringLiteral("Filter env"));
  flt(FilterEnvAttack, 0.001f, 5.f, 0.001f, QStringLiteral("Filter env attack"));
  flt(FilterEnvDecay, 0.001f, 5.f, 0.15f, QStringLiteral("Filter env decay"));
  flt(FilterEnvSustain, 0.f, 1.f, 0.f, QStringLiteral("Filter env sustain"));
  flt(FilterEnvRelease, 0.001f, 8.f, 0.05f, QStringLiteral("Filter env release"));
  flt(VelToCutoff, 0.f, 1.f, 0.f, QStringLiteral("Vel > cutoff"));

  flt(VelAmount, 0.f, 1.f, 1.f, QStringLiteral("Vel > volume"));
  combo(
      VelCurve,
      {{QStringLiteral("Linear"), 0},
       {QStringLiteral("Soft"), 1},
       {QStringLiteral("Hard"), 2}},
      0, QStringLiteral("Vel curve"));
  flt(VelToStart, 0.f, 1.f, 0.f, QStringLiteral("Vel > start"));
  flt(VelXfade, 0.f, 1.f, 0.f, QStringLiteral("Layer crossfade"));

  combo(
      VoiceMode,
      {{QStringLiteral("Poly"), 0},
       {QStringLiteral("Mono"), 1},
       {QStringLiteral("Legato"), 2}},
      0, QStringLiteral("Voices"));
  flt(Glide, 0.f, 2.f, 0.f, QStringLiteral("Glide"));
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

  combo(
      LfoDest,
      {{QStringLiteral("Off"), 0},
       {QStringLiteral("Pitch"), 1},
       {QStringLiteral("Cutoff"), 2},
       {QStringLiteral("Volume"), 3},
       {QStringLiteral("Pan"), 4}},
      0, QStringLiteral("LFO"));
  flt(LfoRate, 0.01f, 40.f, 5.f, QStringLiteral("LFO rate"));
  flt(LfoDepth, 0.f, 1.f, 0.f, QStringLiteral("LFO depth"));
  flt(LfoDelay, 0.f, 5.f, 0.f, QStringLiteral("LFO delay"));

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

  return v;
}

// Applies one control's ossia value onto the parameter block; index is a
// SamplerControl. Invalid values are ignored field-by-field.
void applySamplerControl(SamplerParams& p, int control, const ossia::value& val);
}
