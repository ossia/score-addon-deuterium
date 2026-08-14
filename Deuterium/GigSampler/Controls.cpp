#include <Deuterium/GigSampler/Controls.hpp>

#include <ossia/network/value/value_conversion.hpp>

namespace Deuterium::Gig
{
void applySamplerControl(SamplerParams& p, int control, const ossia::value& val)
{
  const auto flt = [&](float& field, float lo, float hi) {
    const float x = ossia::convert<float>(val);
    if(std::isfinite(x))
      field = std::clamp(x, lo, hi);
  };
  const auto num = [&](int& field, int lo, int hi) {
    field = std::clamp(ossia::convert<int>(val), lo, hi);
  };

  switch(control)
  {
    case Volume:
      flt(p.volume, 0.f, 2.f);
      break;
    case Pan:
      flt(p.pan, -1.f, 1.f);
      break;
    case Transpose:
      flt(p.transpose, -48.f, 48.f);
      break;
    case FineTune:
      flt(p.fineTune, -100.f, 100.f);
      break;
    case BendRange:
      flt(p.bendRange, 0.f, 48.f);
      break;
    case Attack:
      flt(p.attack, -1.f, 60.f);
      break;
    case Decay:
      flt(p.decay, -1.f, 60.f);
      break;
    case Sustain:
      flt(p.sustain, -1.f, 1.f);
      break;
    case Release:
      flt(p.release, -1.f, 60.f);
      break;
    case FilterType:
      num(p.filterType, 0, SamplerParams::FilterNotch);
      break;
    case Cutoff:
      flt(p.cutoff, 20.f, 20000.f);
      break;
    case Resonance:
      flt(p.resonance, 0.f, 1.f);
      break;
    case FilterKeytrack:
      flt(p.filterKeytrack, 0.f, 1.f);
      break;
    case FilterEnvAmount:
      flt(p.filterEnvAmount, -1.f, 1.f);
      break;
    case FilterEnvDecay:
      flt(p.filterEnvDecay, 0.001f, 60.f);
      break;
    case VelToCutoff:
      flt(p.velToCutoff, 0.f, 1.f);
      break;
    case VelAmount:
      flt(p.velAmount, 0.f, 1.f);
      break;
    case VelToStart:
      flt(p.velToStart, 0.f, 1.f);
      break;
    case VelXfade:
      flt(p.velXfade, 0.f, 1.f);
      break;
    case VoiceMode:
      num(p.voiceMode, 0, SamplerParams::Legato);
      break;
    case Glide:
      flt(p.glide, 0.f, 30.f);
      break;
    case Polyphony:
      num(p.polyphony, 1, 64);
      break;
    case StartOffset:
      flt(p.startOffset, 0.f, 1.f);
      break;
    case Reverse:
      p.reverse = ossia::convert<bool>(val);
      break;
    case LoopMode:
      num(p.loopMode, 0, SamplerParams::LoopUntilRelease);
      break;
    case LoopXfade:
      flt(p.loopXfade, 0.f, 5.f);
      break;
    case Lofi:
      flt(p.lofi, 0.f, 1.f);
      break;
    case PitchEnvAmount:
      flt(p.pitchEnvAmount, -48.f, 48.f);
      break;
    case PitchEnvDecay:
      flt(p.pitchEnvDecay, 0.001f, 30.f);
      break;
    case LfoDest:
      num(p.lfoDest, 0, SamplerParams::LfoPan);
      break;
    case LfoRate:
      flt(p.lfoRate, 0.f, 100.f);
      break;
    case LfoDepth:
      flt(p.lfoDepth, 0.f, 1.f);
      break;
    case LfoDelay:
      flt(p.lfoDelay, 0.f, 30.f);
      break;
    case RoundRobin:
      num(p.roundRobin, 0, SamplerParams::RRRandom);
      break;
    default:
      break;
  }
}
}
