#pragma once
#include <Deuterium/GigSampler/GigLoader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure, allocation-free DSP helpers for the sampler voice engine.
// Everything in this header is deterministic and unit-tested; the executor
// only wires MIDI / control values into these functions.
namespace Deuterium::Gig
{

// Global playback parameters, mirrored from the process control inlets
struct SamplerParams
{
  float volume{1.f}; // linear gain
  float pan{0.f};    // -1..1, added to the region pan

  float transpose{0.f}; // semitones
  float fineTune{0.f};  // cents
  float bendRange{2.f}; // semitones for a full pitch wheel

  // Envelope override: values < 0 mean "use the region's own value"
  float attack{-1.f};
  float decay{-1.f};
  float sustain{-1.f};
  float release{-1.f};

  enum FilterType
  {
    FilterFromFile,
    FilterOff,
    FilterLowpass,
    FilterHighpass,
    FilterBandpass,
    FilterNotch
  };
  int filterType{FilterFromFile};
  float cutoff{18000.f};      // Hz, for the global filter types
  float resonance{0.f};       // 0..1
  float filterKeytrack{0.f};  // 0..1: cutoff follows the key
  float filterEnvAmount{0.f}; // -1..1, scaled to +-4 octaves
  float filterEnvAttack{0.001f};
  float filterEnvDecay{0.15f};
  // Sustain 0 so that the decay stage actually sweeps: the envelope opens the
  // filter by `filterEnvAmount` and closes again over `filterEnvDecay`
  float filterEnvSustain{0.f};
  float filterEnvRelease{0.05f};
  float velToCutoff{0.f}; // 0..1

  float velAmount{1.f};  // 0..1: how much velocity scales the gain
  int velCurve{2};       // 0 linear, 1 soft (sqrt), 2 hard (v^2, the SF2
                         // default velocity-to-attenuation curve)
  float velToStart{0.f}; // 0..1: softer hits start later in the sample
  float velXfade{0.f};   // 0..1: crossfade across velocity-zone edges

  enum VoiceMode
  {
    Poly,
    Mono,
    Legato
  };
  int voiceMode{Poly};
  float glide{0.f}; // seconds, mono/legato only
  int polyphony{64};

  float startOffset{0.f}; // 0..1 of the sample length
  bool reverse{false};

  enum LoopMode
  {
    LoopFromFile,
    LoopOff,
    LoopForward,
    LoopPingPong,
    LoopUntilRelease
  };
  int loopMode{LoopFromFile};
  float loopXfade{0.f}; // seconds of crossfade at the loop seam

  float lofi{0.f}; // 0..1: one-knob rate + bit-depth reduction

  float pitchEnvAmount{0.f};  // semitones, bipolar
  float pitchEnvDecay{0.08f}; // seconds

  enum LfoDest
  {
    LfoOff,
    LfoPitch,
    LfoCutoff,
    LfoAmp,
    LfoPan
  };
  int lfoDest{LfoOff};
  float lfoRate{5.f};  // Hz
  float lfoDepth{0.f}; // 0..1 (semitones for pitch, octaves for cutoff...)
  float lfoDelay{0.f}; // seconds of fade-in delay

  enum RoundRobinMode
  {
    RRFromFile,
    RROff,
    RRCycle,
    RRRandom
  };
  int roundRobin{RRFromFile};

  // Chromatic mode: every key plays the region(s) mapped at chromaticRoot,
  // repitched by the distance to the root (MPC 16-levels / SP multipitch)
  bool chromatic{false};
  int chromaticRoot{60};
};

inline double semitonesToRatio(double st) noexcept
{
  return std::exp2(st / 12.0);
}

// Gain from velocity: lerp(1, velocity, amount), disabled by the region's
// applyVelocity flag (Hydrogen kits use it for fixed-level drums)
inline double velocityGain(const GigRegion& r, const SamplerParams& p, int velocity) noexcept
{
  if(!r.applyVelocity)
    return 1.0;
  double v = std::clamp(velocity, 0, 127) / 127.0;
  if(p.velCurve == 1)
    v = std::sqrt(v); // soft: quiet hits come up
  else if(p.velCurve == 2)
    v = v * v; // hard: exaggerated dynamics
  const double amount = std::clamp<double>(p.velAmount, 0., 1.);
  return 1.0 + amount * (v - 1.0);
}

// Zone-edge gain: 1 inside the velocity zone, fading to 0 across up to 32
// velocity units around it when velXfade > 0 (soft layer switching)
inline double velocityZoneGain(const GigRegion& r, int velocity, double xfade01) noexcept
{
  const bool inside = velocity >= r.velLow && velocity <= r.velHigh;
  if(xfade01 <= 0.)
    return inside ? 1. : 0.;
  const double w = std::clamp(xfade01, 0., 1.) * 32.;
  double distance = 0.;
  if(velocity < r.velLow)
    distance = r.velLow - velocity;
  else if(velocity > r.velHigh)
    distance = velocity - r.velHigh;
  return std::max(0., 1. - distance / w);
}

// Static pitch of a note on a region under the params, in semitones
// (live modulation - bend, LFO, pitch envelope, glide - comes on top)
inline double
basePitchSemitones(const GigRegion& r, const SamplerParams& p, int note) noexcept
{
  double st = r.pitchOffset + r.sample.fineTune / 100.0 + p.transpose
              + p.fineTune / 100.0;
  if(r.pitchTrack)
    st += (note - (int)r.sample.midiUnityNote) * r.keyScale;
  return st;
}

// Amplitude envelope resolution: the global override wins when set (>= 0)
struct ResolvedEnv
{
  double attack, decay, sustain, release;
};
inline ResolvedEnv resolveEnvelope(const GigRegion& r, const SamplerParams& p) noexcept
{
  return {
      p.attack >= 0.f ? (double)p.attack : r.eg1Attack,
      p.decay >= 0.f ? (double)p.decay : r.eg1Decay,
      p.sustain >= 0.f ? std::clamp<double>(p.sustain, 0., 1.) : r.eg1Sustain,
      p.release >= 0.f ? (double)p.release : r.eg1Release};
}

// Loop resolution against the decoded frame count. mode: 0 off, 1 forward,
// 2 ping-pong, 3 forward-until-release
struct ResolvedLoop
{
  int mode{};
  int64_t start{};
  int64_t end{};
};
inline ResolvedLoop
resolveLoop(const GigRegion& r, const SamplerParams& p, int64_t frames) noexcept
{
  ResolvedLoop loop{0, 0, frames};
  const bool fileLoop = r.sample.hasLoop && r.sample.loopEnd > r.sample.loopStart
                        && (int64_t)r.sample.loopEnd <= frames;
  switch(p.loopMode)
  {
    case SamplerParams::LoopFromFile:
      loop.mode = fileLoop ? (r.sample.loopUntilRelease ? 3 : 1) : 0;
      break;
    case SamplerParams::LoopOff:
      loop.mode = 0;
      break;
    case SamplerParams::LoopForward:
      loop.mode = 1;
      break;
    case SamplerParams::LoopPingPong:
      loop.mode = 2;
      break;
    case SamplerParams::LoopUntilRelease:
      loop.mode = 3;
      break;
    default:
      loop.mode = 0;
      break;
  }
  if(loop.mode != 0)
  {
    if(fileLoop)
    {
      loop.start = r.sample.loopStart;
      loop.end = r.sample.loopEnd;
    }
    if(loop.end <= loop.start || loop.end > frames)
      loop.mode = 0;
  }
  return loop;
}

// Fractional playback head with direction, looping and boundary handling.
// advance() returns false when the voice reached its end.
struct PlayHead
{
  double pos{};
  int dir{1};

  bool advance(double ratio, const ResolvedLoop& loop, int64_t frames,
               bool released) noexcept
  {
    pos += dir * ratio;

    const bool looping = loop.mode == 1 || loop.mode == 2
                         || (loop.mode == 3 && !released);
    if(looping)
    {
      if(loop.mode == 2)
      {
        // Ping-pong: reflect at both loop boundaries
        if(dir > 0 && pos >= loop.end - 1)
        {
          pos = (loop.end - 1) - (pos - (loop.end - 1));
          dir = -1;
        }
        else if(dir < 0 && pos < loop.start)
        {
          pos = loop.start + (loop.start - pos);
          dir = 1;
        }
        // Degenerate loops: keep the head inside
        pos = std::clamp<double>(pos, 0., std::max<int64_t>(0, frames - 1));
        return true;
      }
      const double len = loop.end - loop.start;
      if(dir > 0 && pos >= loop.end)
        pos = loop.start + std::fmod(pos - loop.start, len);
      else if(dir < 0 && pos < loop.start)
        pos = loop.end - std::fmod(loop.start - pos, len);
      return true;
    }

    return pos >= 0. && pos < (double)frames;
  }
};

// Sample read with loop-seam crossfade: when the head is within xfadeFrames
// of a forward loop's end, the previous pass of the loop is blended in.
struct SampleRead
{
  int64_t idx{};
  double frac{};
  double xfadeWeight{}; // 0 = no blend; else weight of the seam partner
  int64_t xfadeIdx{};
};
inline SampleRead readPosition(
    double pos, int64_t frames, const ResolvedLoop& loop, int64_t xfadeFrames,
    bool released) noexcept
{
  SampleRead sr;
  pos = std::clamp(pos, 0., (double)(frames > 0 ? frames - 1 : 0));
  sr.idx = (int64_t)pos;
  sr.frac = pos - sr.idx;

  const bool crossfadable
      = xfadeFrames > 0 && (loop.mode == 1 || (loop.mode == 3 && !released));
  if(crossfadable)
  {
    // The seam is blended with the material just before the loop start, so
    // there must be at least xfadeFrames of pre-loop audio
    const int64_t len = loop.end - loop.start;
    if(len > xfadeFrames && loop.start >= xfadeFrames
       && sr.idx >= loop.end - xfadeFrames && sr.idx < loop.end)
    {
      sr.xfadeWeight = double(sr.idx - (loop.end - xfadeFrames)) / (double)xfadeFrames;
      sr.xfadeIdx = sr.idx - len;
    }
  }
  return sr;
}

// RBJ biquad (audio-eq-cookbook), per-voice, coefficients recomputed per
// block. Replaces the heavyweight external filter and supports LP/HP/BP/notch.
struct Biquad
{
  double b0{1.}, b1{}, b2{}, a1{}, a2{};
  double x1{}, x2{}, y1{}, y2{};
  double x1r{}, x2r{}, y1r{}, y2r{};

  void reset() noexcept { x1 = x2 = y1 = y2 = x1r = x2r = y1r = y2r = 0.; }

  // type: SamplerParams::FilterLowpass..FilterNotch. q >= 0.5 recommended.
  // `gain` scales the numerator: file-driven filters use it to take part of
  // the resonance peak out of the passband (SF2 convention).
  void configure(
      int type, double cutoffHz, double q, double sampleRate,
      double gain = 1.) noexcept
  {
    cutoffHz = std::clamp(cutoffHz, 10., sampleRate * 0.49);
    q = std::clamp(q, 0.1, 40.);
    const double w0 = 2. * M_PI * cutoffHz / sampleRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2. * q);
    double B0, B1, B2, A0, A1, A2;
    switch(type)
    {
      default:
      case SamplerParams::FilterLowpass:
        B0 = (1. - cw) / 2.;
        B1 = 1. - cw;
        B2 = B0;
        break;
      case SamplerParams::FilterHighpass:
        B0 = (1. + cw) / 2.;
        B1 = -(1. + cw);
        B2 = B0;
        break;
      case SamplerParams::FilterBandpass:
        B0 = alpha;
        B1 = 0.;
        B2 = -alpha;
        break;
      case SamplerParams::FilterNotch:
        B0 = 1.;
        B1 = -2. * cw;
        B2 = 1.;
        break;
    }
    A0 = 1. + alpha;
    A1 = -2. * cw;
    A2 = 1. - alpha;
    b0 = gain * B0 / A0;
    b1 = gain * B1 / A0;
    b2 = gain * B2 / A0;
    a1 = A1 / A0;
    a2 = A2 / A0;
  }

  void process(double& l, double& r) noexcept
  {
    // The tiny offset keeps the recursion out of denormal territory
    double y = b0 * l + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2 + 1e-24;
    x2 = x1;
    x1 = l;
    y2 = y1;
    y1 = y;
    l = y;

    y = b0 * r + b1 * x1r + b2 * x2r - a1 * y1r - a2 * y2r + 1e-24;
    x2r = x1r;
    x1r = r;
    y2r = y1r;
    y1r = y;
    r = y;
  }
};

// One-knob lo-fi: sample-and-hold rate reduction plus bit quantization.
// amount 0 is bit-transparent; 1 is ~1/8 rate and 6 bits.
struct LofiState
{
  double heldL{}, heldR{};
  double phase{1.};

  void process(double amount, double& l, double& r) noexcept
  {
    if(amount <= 0.)
      return;
    amount = std::min(amount, 1.);
    phase += 1.0 - 0.875 * amount;
    if(phase >= 1.)
    {
      phase -= 1.;
      heldL = l;
      heldR = r;
    }
    const double bits = 16.0 - 10.0 * amount;
    const double q = std::exp2(bits - 1.0);
    l = std::round(heldL * q) / q;
    r = std::round(heldR * q) / q;
  }
};

// Linear ADSR advanced once per block (for control-rate modulation such as
// the filter envelope; the amplitude envelope stays per-sample).
struct BlockEnv
{
  enum Stage
  {
    Idle,
    Attack,
    Decay,
    Sustain,
    Release
  };
  int stage{Idle};
  double value{};

  void trigger() noexcept
  {
    stage = Attack;
    value = 0.;
  }
  void release() noexcept
  {
    if(stage != Idle)
      stage = Release;
  }

  double advanceBlock(
      double blockFrames, double a, double d, double s, double r,
      double sampleRate) noexcept
  {
    s = std::clamp(s, 0., 1.);
    const auto frames = [&](double seconds) {
      return std::max(1.0, seconds * sampleRate);
    };
    switch(stage)
    {
      case Attack:
        value += blockFrames / frames(a);
        if(value >= 1.)
        {
          value = 1.;
          stage = Decay;
        }
        break;
      case Decay:
        value -= blockFrames * (1. - s) / frames(d);
        if(value <= s)
        {
          value = s;
          stage = Sustain;
        }
        break;
      case Sustain:
        value = s;
        break;
      case Release:
        value -= blockFrames / frames(r);
        if(value <= 0.)
        {
          value = 0.;
          stage = Idle;
        }
        break;
      default:
        break;
    }
    return value;
  }
};

// Exponential-decay pitch envelope: starts at `amount` semitones, decays
// towards 0 with the given time constant. Advanced once per block.
struct PitchEnv
{
  double value{};
  void trigger(double amountSemitones) noexcept { value = amountSemitones; }
  void advanceBlock(double blockFrames, double decaySeconds, double sampleRate) noexcept
  {
    if(value == 0.)
      return;
    const double tau = std::max(1e-3, (double)decaySeconds) * sampleRate;
    value *= std::exp(-blockFrames / tau);
    if(std::abs(value) < 1e-4)
      value = 0.;
  }
};

// Delayed sine LFO, advanced once per block; output in -1..1 scaled by the
// fade-in envelope after `delay` seconds.
struct Lfo
{
  double phase{};
  double age{}; // seconds since note start

  double advanceBlock(
      double blockFrames, double rateHz, double delaySeconds,
      double sampleRate) noexcept
  {
    const double dt = blockFrames / sampleRate;
    age += dt;
    phase += rateHz * dt;
    phase -= std::floor(phase);
    double fade = 1.;
    if(delaySeconds > 0.)
      fade = std::clamp((age - delaySeconds) / std::max(0.05, delaySeconds * 0.5), 0., 1.);
    return std::sin(2. * M_PI * phase) * fade;
  }
};

// Round-robin pick among `count` alternatives of one zone.
// policy: RRCycle or RRRandom; `counter` is per-note persistent state and
// `rng` a xorshift32 state.
inline int
pickAlternative(int count, int policy, uint8_t& counter, uint32_t& rng) noexcept
{
  if(count <= 1)
    return 0;
  if(policy == SamplerParams::RRRandom)
  {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return int(rng % uint32_t(count));
  }
  // cycle
  const int r = counter % count;
  counter = uint8_t((counter + 1) % count);
  return r;
}

// Glide: linear semitone slew from the previous note's pitch
struct GlideState
{
  double current{};
  double target{};
  bool active{};

  void start(double from, double to, bool glideEnabled) noexcept
  {
    target = to;
    if(glideEnabled)
    {
      current = from;
      active = true;
    }
    else
    {
      current = to;
      active = false;
    }
  }

  double advanceBlock(double blockFrames, double glideSeconds, double sampleRate) noexcept
  {
    if(!active || glideSeconds <= 0.)
    {
      current = target;
      active = false;
      return current;
    }
    const double step = 12.0 * blockFrames / (glideSeconds * sampleRate);
    if(std::abs(target - current) <= step)
    {
      current = target;
      active = false;
    }
    else
    {
      current += (target > current) ? step : -step;
    }
    return current;
  }
};

}
