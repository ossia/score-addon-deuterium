#pragma once
#include <ossia/dataflow/nodes/media.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Deuterium::Gig
{

struct GigSample
{
  ossia::audio_array data;
  uint32_t sampleRate{44100};
  uint32_t midiUnityNote{60};
  uint32_t fineTune{0};
  bool hasLoop{false};
  uint32_t loopStart{0};
  uint32_t loopEnd{0};
  int loopType{0}; // 0=forward, 1=bidirectional, 2=backward
};

struct GigRegion
{
  uint8_t keyLow{0};
  uint8_t keyHigh{127};
  uint8_t velLow{0};
  uint8_t velHigh{127};

  // EG1 (amplitude)
  double eg1Attack{0.0};
  double eg1Decay{0.0};
  double eg1Sustain{1.0};
  double eg1Release{0.05};

  // Filter
  bool vcfEnabled{false};
  uint8_t vcfCutoff{127};
  uint8_t vcfResonance{0};

  // Playback
  bool pitchTrack{true};
  int8_t pan{0}; // -64..63
  double sampleAttenuation{1.0};
  uint16_t sampleStartOffset{0};

  GigSample sample;
};

struct GigInstrument
{
  std::string name;
  int32_t attenuation{0};
  uint16_t pitchbendRange{2};
  std::vector<GigRegion> regions;
};

struct GigFileInfo
{
  std::string name;
  std::string filePath;
  std::vector<GigInstrument> instruments;
  int selectedInstrument{0};
};

// Phase 1: Fast metadata parse (GUI-safe).
// Returns a GigFileInfo with all region/instrument metadata populated
// but with empty sample data arrays.
std::shared_ptr<GigFileInfo>
loadGigFileMetadata(const QString& filePath, int instrumentIndex = 0);

// Phase 2: Load and convert all sample data (slow, run on worker thread).
// Fills in sample.data for every region of the selected instrument.
// Checks cancelled token periodically and aborts early if set.
// Returns a new GigFileInfo with fully populated sample data.
std::shared_ptr<GigFileInfo> loadGigFileSamples(
    const std::shared_ptr<GigFileInfo>& metadata,
    int targetRate,
    std::shared_ptr<std::atomic<bool>> cancelled);

}
