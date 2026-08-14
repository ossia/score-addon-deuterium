// Unit tests for the pure DSP helpers of the sampler engine: pitch and gain
// math, loop stepping, biquad filter, lo-fi, envelopes, LFO, glide and
// round-robin selection. Everything must stay finite for hostile inputs.
#include <Deuterium/GigSampler/SamplerEngine.hpp>

#include <QtTest>

using namespace Deuterium::Gig;

class SamplerEngineTest final : public QObject
{
  Q_OBJECT

  static GigRegion makeRegion()
  {
    GigRegion r;
    r.sample.midiUnityNote = 60;
    return r;
  }

private Q_SLOTS:

  void test_pitch_math()
  {
    QCOMPARE(semitonesToRatio(0.), 1.);
    QCOMPARE(semitonesToRatio(12.), 2.);
    QVERIFY(std::abs(semitonesToRatio(-12.) - 0.5) < 1e-12);

    auto r = makeRegion();
    SamplerParams p;
    QCOMPARE(basePitchSemitones(r, p, 72), 12.);

    p.transpose = 7.f;
    p.fineTune = 50.f;
    QVERIFY(std::abs(basePitchSemitones(r, p, 60) - 7.5) < 1e-9);

    r.pitchTrack = false;
    QVERIFY(std::abs(basePitchSemitones(r, p, 100) - 7.5) < 1e-9);

    r.sample.fineTune = -100; // cents
    QVERIFY(std::abs(basePitchSemitones(r, p, 100) - 6.5) < 1e-9);
  }

  void test_velocity_gain()
  {
    auto r = makeRegion();
    SamplerParams p;
    p.velAmount = 1.f;
    QVERIFY(std::abs(velocityGain(r, p, 127) - 1.) < 1e-9);
    QVERIFY(std::abs(velocityGain(r, p, 0) - 0.) < 1e-9);

    p.velAmount = 0.f;
    QVERIFY(std::abs(velocityGain(r, p, 0) - 1.) < 1e-9);

    p.velAmount = 1.f;
    r.applyVelocity = false;
    QVERIFY(std::abs(velocityGain(r, p, 0) - 1.) < 1e-9);
  }

  void test_velocity_zone_crossfade()
  {
    auto r = makeRegion();
    r.velLow = 64;
    r.velHigh = 100;

    // Hard edges without crossfade
    QCOMPARE(velocityZoneGain(r, 63, 0.), 0.);
    QCOMPARE(velocityZoneGain(r, 64, 0.), 1.);
    QCOMPARE(velocityZoneGain(r, 100, 0.), 1.);
    QCOMPARE(velocityZoneGain(r, 101, 0.), 0.);

    // With crossfade: still 1 inside, fading outside, monotonic
    QCOMPARE(velocityZoneGain(r, 80, 1.), 1.);
    const double g1 = velocityZoneGain(r, 60, 1.);
    const double g2 = velocityZoneGain(r, 40, 1.);
    QVERIFY(g1 > 0. && g1 < 1.);
    QVERIFY(g2 < g1);
    QCOMPARE(velocityZoneGain(r, 0, 1.), 0.);
  }

  void test_envelope_override()
  {
    auto r = makeRegion();
    r.eg1Attack = 0.5;
    r.eg1Sustain = 0.7;
    SamplerParams p; // all overrides < 0 by default

    auto e = resolveEnvelope(r, p);
    QCOMPARE(e.attack, 0.5);
    QCOMPARE(e.sustain, 0.7);

    p.attack = 0.f;
    p.sustain = 1.f;
    e = resolveEnvelope(r, p);
    QCOMPARE(e.attack, 0.);
    QCOMPARE(e.sustain, 1.);
  }

  void test_loop_resolution()
  {
    auto r = makeRegion();
    SamplerParams p;

    // No file loop, from-file mode: off
    QCOMPARE(resolveLoop(r, p, 1000).mode, 0);

    // Valid file loop
    r.sample.hasLoop = true;
    r.sample.loopStart = 100;
    r.sample.loopEnd = 900;
    auto loop = resolveLoop(r, p, 1000);
    QCOMPARE(loop.mode, 1);
    QCOMPARE(loop.start, int64_t(100));
    QCOMPARE(loop.end, int64_t(900));

    // Forced off
    p.loopMode = SamplerParams::LoopOff;
    QCOMPARE(resolveLoop(r, p, 1000).mode, 0);

    // Forced forward without a file loop: whole sample
    r.sample.hasLoop = false;
    p.loopMode = SamplerParams::LoopForward;
    loop = resolveLoop(r, p, 1000);
    QCOMPARE(loop.mode, 1);
    QCOMPARE(loop.start, int64_t(0));
    QCOMPARE(loop.end, int64_t(1000));

    // A file loop beyond the data cannot be used
    r.sample.hasLoop = true;
    r.sample.loopEnd = 5000;
    p.loopMode = SamplerParams::LoopFromFile;
    QCOMPARE(resolveLoop(r, p, 1000).mode, 0);
  }

  void test_playhead_forward_loop()
  {
    ResolvedLoop loop{1, 100, 200};
    PlayHead head{190., 1};
    for(int i = 0; i < 1000; i++)
    {
      QVERIFY(head.advance(7.3, loop, 1000, false));
      QVERIFY(head.pos >= 100.);
      QVERIFY(head.pos < 200.);
    }
  }

  void test_playhead_pingpong_stays_inside()
  {
    ResolvedLoop loop{2, 100, 200};
    PlayHead head{150., 1};
    bool sawReverse = false;
    for(int i = 0; i < 5000; i++)
    {
      QVERIFY(head.advance(3.7, loop, 1000, false));
      QVERIFY(head.pos >= 0.);
      QVERIFY(head.pos <= 999.);
      if(head.dir < 0)
        sawReverse = true;
    }
    QVERIFY(sawReverse);
  }

  void test_playhead_until_release()
  {
    ResolvedLoop loop{3, 100, 200};
    PlayHead head{150., 1};
    for(int i = 0; i < 100; i++)
      QVERIFY(head.advance(5., loop, 300, false));
    QVERIFY(head.pos < 200.);

    // After release the loop opens and the head runs to the end
    bool ended = false;
    for(int i = 0; i < 100; i++)
      if(!head.advance(5., loop, 300, true))
      {
        ended = true;
        break;
      }
    QVERIFY(ended);
  }

  void test_playhead_reverse_ends()
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
    QVERIFY(ended);
  }

  void test_loop_crossfade_weights()
  {
    ResolvedLoop loop{1, 100, 500};
    // Inside the crossfade window the weight ramps up; before it, zero
    auto sr0 = readPosition(200., 1000, loop, 50, false);
    QCOMPARE(sr0.xfadeWeight, 0.);
    auto sr1 = readPosition(460., 1000, loop, 50, false);
    QVERIFY(sr1.xfadeWeight > 0. && sr1.xfadeWeight < 1.);
    auto sr2 = readPosition(490., 1000, loop, 50, false);
    QVERIFY(sr2.xfadeWeight > sr1.xfadeWeight);
    // The blend partner sits just before the loop start, never negative
    QVERIFY(sr1.xfadeIdx >= 0);
    QVERIFY(sr1.xfadeIdx < loop.start);

    // Loops starting at 0 have no pre-loop material: no crossfade
    ResolvedLoop loop0{1, 0, 500};
    auto sr3 = readPosition(480., 1000, loop0, 50, false);
    QCOMPARE(sr3.xfadeWeight, 0.);
  }

  void test_biquad_lowpass_and_highpass()
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
      QVERIFY(std::isfinite(l) && std::isfinite(r));
    }
    QVERIFY(std::abs(l - 1.) < 0.05);

    Biquad hp;
    hp.configure(SamplerParams::FilterHighpass, 1000., 0.707, 48000.);
    for(int i = 0; i < 4000; i++)
    {
      l = 1.;
      r = 1.;
      hp.process(l, r);
    }
    QVERIFY(std::abs(l) < 0.05);
  }

  void test_biquad_stability_extremes()
  {
    Biquad f;
    // Hostile configurations must clamp and stay stable
    f.configure(SamplerParams::FilterBandpass, 0., 1000., 48000.);
    double peak = 0;
    for(int i = 0; i < 20000; i++)
    {
      double l = (i % 7) / 3.5 - 1., r = l;
      f.process(l, r);
      QVERIFY(std::isfinite(l));
      peak = std::max(peak, std::abs(l));
    }
    QVERIFY(peak < 1e3);
  }

  void test_lofi()
  {
    LofiState lofi;
    // Zero amount is exactly transparent
    double l = 0.123456789, r = -0.5;
    lofi.process(0., l, r);
    QCOMPARE(l, 0.123456789);
    QCOMPARE(r, -0.5);

    // Full amount quantizes to a 6 bit grid and holds samples
    LofiState lofi2;
    int changes = 0;
    double prev = 999.;
    for(int i = 0; i < 64; i++)
    {
      double v = std::sin(i * 0.7), dummy = v;
      lofi2.process(1., v, dummy);
      QVERIFY(std::isfinite(v));
      // On the 6-bit grid
      const double q = std::exp2(5.);
      QVERIFY(std::abs(v * q - std::round(v * q)) < 1e-9);
      if(v != prev)
        changes++;
      prev = v;
    }
    // Held: far fewer output changes than input samples
    QVERIFY(changes < 20);
  }

  void test_block_env()
  {
    BlockEnv env;
    env.trigger();
    // Attack to 1
    double v = 0;
    for(int i = 0; i < 100; i++)
      v = env.advanceBlock(64, 0.01, 0.05, 0.5, 0.05, 48000.);
    QVERIFY(env.stage == BlockEnv::Sustain);
    QCOMPARE(v, 0.5);

    env.release();
    for(int i = 0; i < 200; i++)
      v = env.advanceBlock(64, 0.01, 0.05, 0.5, 0.05, 48000.);
    QCOMPARE(v, 0.);
    QVERIFY(env.stage == BlockEnv::Idle);
  }

  void test_pitch_env_decays()
  {
    PitchEnv env;
    env.trigger(12.);
    const double first = env.value;
    for(int i = 0; i < 100; i++)
      env.advanceBlock(64, 0.05, 48000.);
    QVERIFY(std::abs(env.value) < std::abs(first));
    for(int i = 0; i < 2000; i++)
      env.advanceBlock(64, 0.05, 48000.);
    QCOMPARE(env.value, 0.);
  }

  void test_lfo_delay_gates_output()
  {
    Lfo lfo;
    // During the delay the output is silent
    double v = lfo.advanceBlock(64, 5., 1., 48000.);
    QCOMPARE(v, 0.);

    // After the delay it oscillates within -1..1
    Lfo lfo2;
    double peak = 0;
    for(int i = 0; i < 10000; i++)
    {
      const double x = lfo2.advanceBlock(64, 5., 0., 48000.);
      QVERIFY(x >= -1. && x <= 1.);
      peak = std::max(peak, std::abs(x));
    }
    QVERIFY(peak > 0.9);
  }

  void test_glide()
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
    QCOMPARE(g.current, 12.);
    // 12 semitones at 12 st/0.5s -> exactly 0.5s = 375 blocks
    QVERIFY(std::abs(blocks - 375) < 40);

    GlideState g2;
    g2.start(0., 12., false);
    QCOMPARE(g2.advanceBlock(64, 0.5, 48000.), 12.);
  }

  void test_round_robin()
  {
    uint8_t counter = 0;
    uint32_t rng = 12345;

    QCOMPARE(pickAlternative(1, SamplerParams::RRCycle, counter, rng), 0);

    // Cycle: 0, 1, 2, 0, 1, 2...
    for(int round = 0; round < 2; round++)
      for(int i = 0; i < 3; i++)
        QCOMPARE(pickAlternative(3, SamplerParams::RRCycle, counter, rng), i);

    // Random: always in range, not constant
    bool varied = false;
    int first = pickAlternative(4, SamplerParams::RRRandom, counter, rng);
    for(int i = 0; i < 64; i++)
    {
      const int r = pickAlternative(4, SamplerParams::RRRandom, counter, rng);
      QVERIFY(r >= 0 && r < 4);
      if(r != first)
        varied = true;
    }
    QVERIFY(varied);
  }
};

QTEST_APPLESS_MAIN(SamplerEngineTest)
#include "SamplerEngineTest.moc"
