// Unit tests for the pure DSP helpers of the sampler engine: pitch and gain
// math, loop stepping, biquad filter, lo-fi, envelopes, LFO, glide and
// round-robin selection. Everything must stay finite for hostile inputs.
#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/SamplerEngine.hpp>

#include "TestHelpers.hpp"

using namespace Deuterium::Gig;

static GigRegion makeRegion()
{
GigRegion r;
r.sample.midiUnityNote = 60;
return r;
}

TEST_CASE("engine: pitch_math", "[deuterium]")
{
  REQUIRE(approxEq(semitonesToRatio(0.), 1.));
  REQUIRE(approxEq(semitonesToRatio(12.), 2.));
  REQUIRE(std::abs(semitonesToRatio(-12.) - 0.5) < 1e-12);

  auto r = makeRegion();
  SamplerParams p;
  REQUIRE(approxEq(basePitchSemitones(r, p, 72), 12.));

  p.transpose = 7.f;
  p.fineTune = 50.f;
  REQUIRE(std::abs(basePitchSemitones(r, p, 60) - 7.5) < 1e-9);

  r.pitchTrack = false;
  REQUIRE(std::abs(basePitchSemitones(r, p, 100) - 7.5) < 1e-9);

  r.sample.fineTune = -100; // cents
  REQUIRE(std::abs(basePitchSemitones(r, p, 100) - 6.5) < 1e-9);
}

TEST_CASE("engine: velocity_gain", "[deuterium]")
{
  auto r = makeRegion();
  SamplerParams p;
  p.velAmount = 1.f;
  REQUIRE(std::abs(velocityGain(r, p, 127) - 1.) < 1e-9);
  REQUIRE(std::abs(velocityGain(r, p, 0) - 0.) < 1e-9);

  p.velAmount = 0.f;
  REQUIRE(std::abs(velocityGain(r, p, 0) - 1.) < 1e-9);

  p.velAmount = 1.f;
  r.applyVelocity = false;
  REQUIRE(std::abs(velocityGain(r, p, 0) - 1.) < 1e-9);
}

TEST_CASE("engine: velocity_curve", "[deuterium]")
{
  auto r = makeRegion();
  SamplerParams p;
  p.velAmount = 1.f;

  const double lin = velocityGain(r, p, 64);
  p.velCurve = 1; // soft
  const double soft = velocityGain(r, p, 64);
  p.velCurve = 2; // hard
  const double hard = velocityGain(r, p, 64);

  // Soft raises quiet hits, hard lowers them; extremes are unchanged
  REQUIRE(soft > lin);
  REQUIRE(hard < lin);
  for(int curve : {0, 1, 2})
  {
    p.velCurve = curve;
    REQUIRE(std::abs(velocityGain(r, p, 127) - 1.) < 1e-9);
    REQUIRE(std::abs(velocityGain(r, p, 0) - 0.) < 1e-9);
  }
}

TEST_CASE("engine: velocity_zone_crossfade", "[deuterium]")
{
  auto r = makeRegion();
  r.velLow = 64;
  r.velHigh = 100;

  // Hard edges without crossfade
  REQUIRE(approxEq(velocityZoneGain(r, 63, 0.), 0.));
  REQUIRE(approxEq(velocityZoneGain(r, 64, 0.), 1.));
  REQUIRE(approxEq(velocityZoneGain(r, 100, 0.), 1.));
  REQUIRE(approxEq(velocityZoneGain(r, 101, 0.), 0.));

  // With crossfade: still 1 inside, fading outside, monotonic
  REQUIRE(approxEq(velocityZoneGain(r, 80, 1.), 1.));
  const double g1 = velocityZoneGain(r, 60, 1.);
  const double g2 = velocityZoneGain(r, 40, 1.);
  REQUIRE((g1 > 0. && g1 < 1.));
  REQUIRE(g2 < g1);
  REQUIRE(approxEq(velocityZoneGain(r, 0, 1.), 0.));
}

TEST_CASE("engine: envelope_override", "[deuterium]")
{
  auto r = makeRegion();
  r.eg1Attack = 0.5;
  r.eg1Sustain = 0.7;
  SamplerParams p; // all overrides < 0 by default

  auto e = resolveEnvelope(r, p);
  REQUIRE(approxEq(e.attack, 0.5));
  REQUIRE(approxEq(e.sustain, 0.7));

  p.attack = 0.f;
  p.sustain = 1.f;
  e = resolveEnvelope(r, p);
  REQUIRE(approxEq(e.attack, 0.));
  REQUIRE(approxEq(e.sustain, 1.));
}

TEST_CASE("engine: loop_resolution", "[deuterium]")
{
  auto r = makeRegion();
  SamplerParams p;

  // No file loop, from-file mode: off
  REQUIRE(approxEq(resolveLoop(r, p, 1000).mode, 0));

  // Valid file loop
  r.sample.hasLoop = true;
  r.sample.loopStart = 100;
  r.sample.loopEnd = 900;
  auto loop = resolveLoop(r, p, 1000);
  REQUIRE(approxEq(loop.mode, 1));
  REQUIRE(approxEq(loop.start, int64_t(100)));
  REQUIRE(approxEq(loop.end, int64_t(900)));

  // Forced off
  p.loopMode = SamplerParams::LoopOff;
  REQUIRE(approxEq(resolveLoop(r, p, 1000).mode, 0));

  // Forced forward without a file loop: whole sample
  r.sample.hasLoop = false;
  p.loopMode = SamplerParams::LoopForward;
  loop = resolveLoop(r, p, 1000);
  REQUIRE(approxEq(loop.mode, 1));
  REQUIRE(approxEq(loop.start, int64_t(0)));
  REQUIRE(approxEq(loop.end, int64_t(1000)));

  // A file loop beyond the data cannot be used
  r.sample.hasLoop = true;
  r.sample.loopEnd = 5000;
  p.loopMode = SamplerParams::LoopFromFile;
  REQUIRE(approxEq(resolveLoop(r, p, 1000).mode, 0));
}

TEST_CASE("engine: playhead_forward_loop", "[deuterium]")
{
  ResolvedLoop loop{1, 100, 200};
  PlayHead head{190., 1};
  for(int i = 0; i < 1000; i++)
  {
    REQUIRE(head.advance(7.3, loop, 1000, false));
    REQUIRE(head.pos >= 100.);
    REQUIRE(head.pos < 200.);
  }
}

TEST_CASE("engine: playhead_pingpong_stays_inside", "[deuterium]")
{
  ResolvedLoop loop{2, 100, 200};
  PlayHead head{150., 1};
  bool sawReverse = false;
  for(int i = 0; i < 5000; i++)
  {
    REQUIRE(head.advance(3.7, loop, 1000, false));
    REQUIRE(head.pos >= 0.);
    REQUIRE(head.pos <= 999.);
    if(head.dir < 0)
      sawReverse = true;
  }
  REQUIRE(sawReverse);
}

TEST_CASE("engine: playhead_until_release", "[deuterium]")
{
  ResolvedLoop loop{3, 100, 200};
  PlayHead head{150., 1};
  for(int i = 0; i < 100; i++)
    REQUIRE(head.advance(5., loop, 300, false));
  REQUIRE(head.pos < 200.);

  // After release the loop opens and the head runs to the end
  bool ended = false;
  for(int i = 0; i < 100; i++)
    if(!head.advance(5., loop, 300, true))
    {
      ended = true;
      break;
    }
  REQUIRE(ended);
}

TEST_CASE("engine: playhead_reverse_ends", "[deuterium]")
{
  ResolvedLoop loop{0, 0, 300};
  PlayHead head{10., -1};
  bool ended = false;
  for(int i = 0; i < 100; i++)
    if(!head.advance(1., loop, 300, false))
    {
      ended = true;
      break;
    }
  REQUIRE(ended);
}

TEST_CASE("engine: loop_crossfade_weights", "[deuterium]")
{
  ResolvedLoop loop{1, 100, 500};
  // Inside the crossfade window the weight ramps up; before it, zero
  auto sr0 = readPosition(200., 1000, loop, 50, false);
  REQUIRE(approxEq(sr0.xfadeWeight, 0.));
  auto sr1 = readPosition(460., 1000, loop, 50, false);
  REQUIRE((sr1.xfadeWeight > 0. && sr1.xfadeWeight < 1.));
  auto sr2 = readPosition(490., 1000, loop, 50, false);
  REQUIRE(sr2.xfadeWeight > sr1.xfadeWeight);
  // The blend partner sits just before the loop start, never negative
  REQUIRE(sr1.xfadeIdx >= 0);
  REQUIRE(sr1.xfadeIdx < loop.start);

  // Loops starting at 0 have no pre-loop material: no crossfade
  ResolvedLoop loop0{1, 0, 500};
  auto sr3 = readPosition(480., 1000, loop0, 50, false);
  REQUIRE(approxEq(sr3.xfadeWeight, 0.));
}

TEST_CASE("engine: biquad_lowpass_and_highpass", "[deuterium]")
{
  Biquad lp;
  lp.configure(SamplerParams::FilterLowpass, 1000., 0.707, 48000.);
  // DC passes a lowpass
  double l = 0, r = 0;
  for(int i = 0; i < 4000; i++)
  {
    l = 1.;
    r = 1.;
    lp.process(l, r);
    REQUIRE((std::isfinite(l) && std::isfinite(r)));
  }
  REQUIRE(std::abs(l - 1.) < 0.05);

  Biquad hp;
  hp.configure(SamplerParams::FilterHighpass, 1000., 0.707, 48000.);
  for(int i = 0; i < 4000; i++)
  {
    l = 1.;
    r = 1.;
    hp.process(l, r);
  }
  REQUIRE(std::abs(l) < 0.05);
}

TEST_CASE("engine: biquad_stability_extremes", "[deuterium]")
{
  Biquad f;
  // Hostile configurations must clamp and stay stable
  f.configure(SamplerParams::FilterBandpass, 0., 1000., 48000.);
  double peak = 0;
  for(int i = 0; i < 20000; i++)
  {
    double l = (i % 7) / 3.5 - 1., r = l;
    f.process(l, r);
    REQUIRE(std::isfinite(l));
    peak = std::max(peak, std::abs(l));
  }
  REQUIRE(peak < 1e3);
}

TEST_CASE("engine: lofi", "[deuterium]")
{
  LofiState lofi;
  // Zero amount is exactly transparent
  double l = 0.123456789, r = -0.5;
  lofi.process(0., l, r);
  REQUIRE(approxEq(l, 0.123456789));
  REQUIRE(approxEq(r, -0.5));

  // Full amount quantizes to a 6 bit grid and holds samples
  LofiState lofi2;
  int changes = 0;
  double prev = 999.;
  for(int i = 0; i < 64; i++)
  {
    double v = std::sin(i * 0.7), dummy = v;
    lofi2.process(1., v, dummy);
    REQUIRE(std::isfinite(v));
    // On the 6-bit grid
    const double q = std::exp2(5.);
    REQUIRE(std::abs(v * q - std::round(v * q)) < 1e-9);
    if(v != prev)
      changes++;
    prev = v;
  }
  // Held: far fewer output changes than input samples
  REQUIRE(changes < 20);
}

TEST_CASE("engine: block_env", "[deuterium]")
{
  BlockEnv env;
  env.trigger();
  // Attack to 1
  double v = 0;
  for(int i = 0; i < 100; i++)
    v = env.advanceBlock(64, 0.01, 0.05, 0.5, 0.05, 48000.);
  REQUIRE(env.stage == BlockEnv::Sustain);
  REQUIRE(approxEq(v, 0.5));

  env.release();
  for(int i = 0; i < 200; i++)
    v = env.advanceBlock(64, 0.01, 0.05, 0.5, 0.05, 48000.);
  REQUIRE(approxEq(v, 0.));
  REQUIRE(env.stage == BlockEnv::Idle);
}

TEST_CASE("engine: pitch_env_decays", "[deuterium]")
{
  PitchEnv env;
  env.trigger(12.);
  const double first = env.value;
  for(int i = 0; i < 100; i++)
    env.advanceBlock(64, 0.05, 48000.);
  REQUIRE(std::abs(env.value) < std::abs(first));
  for(int i = 0; i < 2000; i++)
    env.advanceBlock(64, 0.05, 48000.);
  REQUIRE(approxEq(env.value, 0.));
}

TEST_CASE("engine: lfo_delay_gates_output", "[deuterium]")
{
  Lfo lfo;
  // During the delay the output is silent
  double v = lfo.advanceBlock(64, 5., 1., 48000.);
  REQUIRE(approxEq(v, 0.));

  // After the delay it oscillates within -1..1
  Lfo lfo2;
  double peak = 0;
  for(int i = 0; i < 10000; i++)
  {
    const double x = lfo2.advanceBlock(64, 5., 0., 48000.);
    REQUIRE((x >= -1. && x <= 1.));
    peak = std::max(peak, std::abs(x));
  }
  REQUIRE(peak > 0.9);
}

TEST_CASE("engine: glide", "[deuterium]")
{
  GlideState g;
  g.start(0., 12., true);
  // Half a second of glide at 48kHz in 64-frame blocks
  int blocks = 0;
  while(g.active && blocks < 10000)
  {
    g.advanceBlock(64, 0.5, 48000.);
    blocks++;
  }
  REQUIRE(approxEq(g.current, 12.));
  // 12 semitones at 12 st/0.5s -> exactly 0.5s = 375 blocks
  REQUIRE(std::abs(blocks - 375) < 40);

  GlideState g2;
  g2.start(0., 12., false);
  REQUIRE(approxEq(g2.advanceBlock(64, 0.5, 48000.), 12.));
}

TEST_CASE("engine: round_robin", "[deuterium]")
{
  uint8_t counter = 0;
  uint32_t rng = 12345;

  REQUIRE(approxEq(pickAlternative(1, SamplerParams::RRCycle, counter, rng), 0));

  // Cycle: 0, 1, 2, 0, 1, 2...
  for(int round = 0; round < 2; round++)
    for(int i = 0; i < 3; i++)
      REQUIRE(approxEq(pickAlternative(3, SamplerParams::RRCycle, counter, rng), i));

  // Random: always in range, not constant
  bool varied = false;
  int first = pickAlternative(4, SamplerParams::RRRandom, counter, rng);
  for(int i = 0; i < 64; i++)
  {
    const int r = pickAlternative(4, SamplerParams::RRRandom, counter, rng);
    REQUIRE((r >= 0 && r < 4));
    if(r != first)
      varied = true;
  }
  REQUIRE(varied);
}

TEST_CASE("engine: tempo_sync_time_resolution", "[deuterium]")
{
  using namespace Deuterium::Gig;
  // A quarter note (x = 0.25) at 120 BPM is half a second
  REQUIRE(approxEq(syncTimeToSeconds(0.25f, 120.), 0.5f));
  // A whole note at 60 BPM is four seconds
  REQUIRE(approxEq(syncTimeToSeconds(1.f, 60.), 4.f));
  // Tempo scales linearly
  REQUIRE(approxEq(syncTimeToSeconds(0.25f, 240.), 0.25f));

  // The time controls are exactly the six timing parameters
  int n = 0;
  for(int i = 0; i < ControlCount; i++)
    n += isTimeControl(i) ? 1 : 0;
  REQUIRE(n == 6);
  REQUIRE(isTimeControl(FilterEnvAttack));
  REQUIRE(isTimeControl(FilterEnvDecay));
  REQUIRE(isTimeControl(FilterEnvRelease));
  REQUIRE(isTimeControl(Glide));
  REQUIRE(isTimeControl(LfoRate));
  REQUIRE(isTimeControl(LfoDelay));
  REQUIRE(!isTimeControl(Attack));
  REQUIRE(!isTimeControl(Cutoff));
}
