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
  int velCurve{3};       // 0 linear, 1 soft (sqrt), 2 hard (v^2, the SF2
                         // default velocity-to-attenuation curve), 3 the
                         // file's own curve (gig), hard otherwise
  float velToStart{0.f}; // 0..1: softer hits start later in the sample
  float velXfade{0.f};   // 0..1: crossfade across velocity-zone edges

  // The fingered modes only glide into a note struck while another key is
  // still held (fingered portamento): detached notes start at their pitch
  enum VoiceMode
  {
    Poly,
    Mono,
    Legato,
    MonoFingered,
    LegatoFingered,
    // Two lines: the lowest and the highest held key, each legato
    Duophonic,
    // Poly, each new note gliding from a previous one (paired by pitch)
    PolyGlide,
    // Poly, each new note continuing the nearest held line (paired by
    // pitch, within the max interval); others start new lines
    PolyLegato
  };
  int voiceMode{Poly};

  // Mono family: which held key sounds
  enum NotePriority
  {
    PriorityLast,
    PriorityHighest,
    PriorityLowest
  };
  int notePriority{PriorityLast};
  // Poly glide / legato: notes further apart are never paired (semitones)
  int maxInterval{12};
  // Poly glide / legato: notes struck within this many seconds form a chord,
  // paired in pitch order with the previous one once the window closes (the
  // notes wait for it); 0 = paired as they come, with the nearest
  float chordWindow{0.f};
  // Poly glide: where new notes glide from
  enum PolyGlideFrom
  {
    GlideFromNearest, // the nearest sounding note, pitch-order pairing
    GlideFromLast     // the last note struck
  };
  int polyGlideFrom{GlideFromNearest};
  // One line per MIDI channel for the mono modes (and pairing within each
  // channel for the poly ones); per-channel pitch bend. MPE.
  bool perChannel{false};
  float glide{0.f}; // seconds; the mono-style and poly glide / legato modes
  enum GlideType
  {
    GlideRateOctave, // Glide = time per octave
    GlideRateTone,   // Glide = time per whole tone
    GlideTime,       // Glide = time of the whole glide
    GlideExponential // analog-style lag reaching 99% after Glide
  };
  int glideType{GlideRateOctave};
  // Mono/legato: fade-out of the voice a new note replaces, in seconds
  float monoFade{0.01f};
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

  float pitchEnvAmount{0.f};
  // Velocity scaling of the pitch envelope amount: 0 = none, towards +1 hard
  // hits get the full amount and soft hits less, towards -1 the inverse
  float velToPitchEnv{0.f};
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

  //! Note-offs do nothing: a note ends with its sample or its envelope, or
  //! when the same key is struck again.
  bool ignoreNoteOff{false};
  //! 1-16: only this channel's messages play; 0: every channel.
  int midiChannel{0};
};

//! Whether a MIDI 1 message passes the channel filter: system messages always
//! do, channel messages when on the channel picked (or any, for 0).
inline bool acceptsChannel(const SamplerParams& p, uint8_t status) noexcept
{
  if(p.midiChannel <= 0 || status < 0x80 || status >= 0xF0)
    return true;
  return (status & 0x0F) + 1 == p.midiChannel;
}

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
  if(p.velCurve == 3 && r.velGainTable)
    v = (*r.velGainTable)[std::clamp(velocity, 0, 127)];
  else if(p.velCurve == 1)
    v = std::sqrt(v); // soft: quiet hits come up
  else if(p.velCurve >= 2)
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

// gig layer crossfade: the controller value goes through the region's
// fade-in / fade-out ramps, then an equal-power curve
inline double gigCrossfadeGain(const GigRegion& r, int value) noexcept
{
  if(r.xfadeSource == -1)
    return 1.;
  int x = std::clamp(value, 0, 127);
  if(r.xfadeInvert)
    x = 127 - x;
  const auto ramp = [](int x, int from, int to) {
    return to > from ? double(x - from) / double(to - from) : 1.;
  };
  double a;
  if(x < r.xfadeInStart)
    a = 0.;
  else if(x < r.xfadeInEnd)
    a = ramp(x, r.xfadeInStart, r.xfadeInEnd);
  else if(x <= r.xfadeOutStart)
    a = 1.;
  else if(x < r.xfadeOutEnd)
    a = 1. - ramp(x, r.xfadeOutStart, r.xfadeOutEnd);
  else
    a = 0.;
  return std::sin(std::clamp(a, 0., 1.) * (M_PI / 2.));
}

// gig cutoff units (0..127) <-> Hz: 20 Hz - 20 kHz, exponentially
inline double gigCutoffToHz(double units) noexcept
{
  return 20.0 * std::pow(1000.0, std::clamp(units, 0., 127.) / 127.0);
}
// Cents per cutoff unit of the mapping above: 1200 * log2(1000) / 127
inline constexpr double gigCentsPerCutoffUnit = 94.15880;

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

// Amplitude envelope: delay - linear-amplitude attack - hold - decay -
// sustain - release, with two decay/release time semantics:
//
// - dB mode (SF2 / DLS, the EMU model): decay and release run at constant
//   dB slopes of 96 dB over their configured time; the decay phase ends
//   when the sustain level is reached, so a shallow sustain decays quickly.
// - linear mode (gig / Hydrogen / user-set ADSR knobs): the decay reaches
//   the sustain level after exactly the decay time and the release reaches
//   silence after exactly the release time, whatever the starting level -
//   the semantics musicians expect from ADSR knobs. The slopes are still
//   exponential (dB-linear), so nothing clicks.
//
// Decay, sustain and release advance by one precomputed multiplication per
// sample - no transcendentals on the audio thread after a stage starts.
struct AmpEnv
{
  static constexpr double full_scale_db = 96.;
  static constexpr double silence_amp = 1e-5; // ~ -100 dB

  enum Stage : uint8_t
  {
    Delay,
    Attack,
    Hold,
    Decay,
    Sustain,
    Release,
    Done
  };

  void set_sample_rate(double r) noexcept { m_rate = std::max(1., r); }
  void delay(double s) noexcept { m_delay = std::max(0., s); }
  void attack(double s) noexcept { m_attack = std::max(0., s); }
  void hold(double s) noexcept { m_hold = std::max(0., s); }
  void decay(double s) noexcept { m_decay = std::max(1e-4, s); }
  void sustain(double lin) noexcept { m_sustain = std::clamp(lin, 0., 1.); }
  void release(double s) noexcept { m_release = std::max(1e-4, s); }
  void dbMode(bool b) noexcept { m_dbMode = b; }
  //! Level the attack starts from (gig EG1 pre-attack), 0..1
  void preAttack(double lin) noexcept { m_preAttack = std::clamp(lin, 0., 1.); }
  //! Fall from the sustain level to silence while the key is held, over this
  //! many seconds (gig EG1 decay 2); < 0 = the sustain level holds
  void decay2(double s) noexcept { m_decay2 = s; }
  //! No sustain: after the decay the envelope goes on to its release by
  //! itself, for notes whose note-off is ignored.
  void oneShot(bool b) noexcept { m_oneShot = b; }
  [[nodiscard]] bool releasing() const noexcept
  {
    return m_stage == Release || m_stage == Done;
  }
  void amp(double) noexcept { } // gam::ADSR API compatibility; peak is 1

  void reset() noexcept
  {
    m_oneShot = false;
    m_preAttack = 0.;
    m_decay2 = -1.;
    m_stage = Delay;
    m_amp = 0.;
    m_counter = 0;
    m_mul = 1.;
  }

  [[nodiscard]] bool done() const noexcept { return m_stage == Done; }

  // Enter the release stage from wherever the envelope currently is
  void startRelease() noexcept
  {
    if(m_stage == Release || m_stage == Done)
      return;
    if(m_amp <= silence_amp)
    {
      m_stage = Done;
      m_amp = 0.;
      return;
    }
    const double relSamples = std::max(1., m_release * m_rate);
    if(m_dbMode)
    {
      // Constant slope: 96 dB over the release time, from the current level
      m_mul = std::pow(10., -full_scale_db / (20. * relSamples));
    }
    else
    {
      // Reach silence after exactly the release time from the current level
      m_mul = std::pow(silence_amp / m_amp, 1. / relSamples);
    }
    m_stage = Release;
  }

  double operator()() noexcept
  {
    switch(m_stage)
    {
      case Delay:
        if(m_counter == 0)
        {
          m_counter = (int64_t)(m_delay * m_rate);
          if(m_counter <= 0)
          {
            m_stage = Attack;
            m_amp = m_preAttack;
            return (*this)();
          }
        }
        if(--m_counter <= 0)
        {
          m_stage = Attack;
          m_amp = m_preAttack;
        }
        return 0.;

      case Attack:
      {
        const double inc = (1. - m_preAttack) / std::max(1., m_attack * m_rate);
        m_amp += inc;
        if(m_amp >= 1.)
        {
          m_amp = 1.;
          m_stage = Hold;
          m_counter = (int64_t)(m_hold * m_rate);
        }
        return m_amp;
      }

      case Hold:
        if(--m_counter <= 0)
          enterDecay();
        return 1.;

      case Decay:
        m_amp *= m_mul;
        if(m_amp <= m_sustainAmp)
        {
          m_amp = m_sustainAmp;
          enterSustain();
          if(m_oneShot)
            startRelease();
        }
        // A near-silent decay tail will never be heard again: free the voice
        if(m_amp <= silence_amp)
        {
          m_stage = m_sustainAmp <= silence_amp ? Done : Sustain;
          if(m_stage == Done)
            m_amp = 0.;
        }
        return m_amp;

      case Sustain:
        if(m_oneShot)
          startRelease();
        else if(m_decay2 >= 0.)
        {
          m_amp *= m_mul;
          if(m_amp <= silence_amp)
          {
            m_amp = 0.;
            m_stage = Done;
          }
        }
        return m_amp;

      case Release:
        m_amp *= m_mul;
        if(m_amp <= silence_amp)
        {
          m_amp = 0.;
          m_stage = Done;
        }
        return m_amp;

      default:
      case Done:
        return 0.;
    }
  }

private:
  void enterDecay() noexcept
  {
    m_sustainAmp = m_sustain;
    if(m_sustainAmp >= 1.)
    {
      m_amp = 1.;
      enterSustain();
      return;
    }
    const double decSamples = std::max(1., m_decay * m_rate);
    if(m_dbMode)
    {
      // The configured time is for the full 96 dB fall; the phase stops at
      // the sustain level (SF2 semantics)
      m_mul = std::pow(10., -full_scale_db / (20. * decSamples));
    }
    else
    {
      // Reach the sustain level after exactly the decay time
      const double target = std::max(m_sustainAmp, silence_amp);
      m_mul = std::pow(target, 1. / decSamples);
    }
    m_stage = Decay;
  }

  void enterSustain() noexcept
  {
    m_stage = Sustain;
    // Decay 2: reach silence after exactly its time from the sustain level
    if(m_decay2 >= 0. && m_amp > silence_amp)
      m_mul = std::pow(silence_amp / m_amp, 1. / std::max(1., m_decay2 * m_rate));
  }

  double m_rate{48000.};
  double m_preAttack{};
  double m_decay2{-1.};
  double m_delay{}, m_attack{0.001}, m_hold{}, m_decay{0.001}, m_release{0.016};
  double m_sustain{1.};
  double m_amp{};
  double m_sustainAmp{1.};
  double m_mul{1.};
  int64_t m_counter{};
  Stage m_stage{Done};
  bool m_dbMode{false};
  bool m_oneShot{false};
};

// lerp(1, vel/127, k) for k in [0,1]; lerp(1, 1 - vel/127, -k) for k < 0
inline double velPitchEnvScale(double velToPitchEnv, int velocity) noexcept
{
  const double v = std::clamp(velocity, 0, 127) / 127.0;
  if(velToPitchEnv > 0.)
    return 1. + velToPitchEnv * (v - 1.);
  if(velToPitchEnv < 0.)
    return 1. + -velToPitchEnv * (-v);
  return 1.;
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

  void trigger(double startLevel = 0.) noexcept
  {
    stage = Attack;
    value = std::clamp(startLevel, 0., 1.);
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

// Glide from the previous note's pitch, in semitones. The glide type says
// what the Glide time means (SamplerParams::GlideType):
// - constant rate: the time for an octave, or for a whole tone; the glide
//   lasts longer the larger the interval
// - constant time: the whole glide, whatever the interval
// - exponential: analog-style lag, fast first then slowing down; the Glide
//   time covers 99% of the interval
struct GlideState
{
  double current{};
  double target{};
  double span{}; // interval at the start, for constant time
  bool active{};

  void start(double from, double to, bool glideEnabled) noexcept
  {
    target = to;
    if(glideEnabled)
    {
      current = from;
      span = std::abs(to - from);
      active = true;
    }
    else
    {
      current = to;
      active = false;
    }
  }

  double advanceBlock(
      double blockFrames, double glideSeconds, double sampleRate,
      int type = 0) noexcept
  {
    if(!active || glideSeconds <= 0. || sampleRate <= 0.)
    {
      current = target;
      active = false;
      return current;
    }
    const double fraction = blockFrames / (glideSeconds * sampleRate);
    if(type == 3) // exponential: e^-ln(100) = 1% left after the Glide time
    {
      current += (target - current) * (1. - std::exp(-std::log(100.) * fraction));
      if(std::abs(target - current) < 0.01) // within a cent
      {
        current = target;
        active = false;
      }
      return current;
    }
    const double step = type == 1   ? 2.0 * fraction  // a tone per Glide time
                        : type == 2 ? span * fraction // the interval
                                    : 12.0 * fraction; // an octave
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
