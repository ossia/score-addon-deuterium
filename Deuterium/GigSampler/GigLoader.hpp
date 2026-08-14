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
  // For samples stored outside the bank file (e.g. Hydrogen drumkit layers):
  // absolute path of the audio file to decode in phase 2
  std::string sourceFile;
  uint32_t sampleRate{44100};
  uint32_t midiUnityNote{60};
  int32_t fineTune{0}; // cents
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

  // Drum-style behavior (Hydrogen kits; also expressible by other formats)
  bool oneShot{false};       // ignore note-off, play until the sample ends
  bool muted{false};         // region loaded but never triggered
  bool applyVelocity{true};  // velocity scales the gain
  double pitchOffset{0.0};   // constant pitch offset, in semitones
  double randomPitch{0.0};   // per-hit random pitch, +/- this many semitones
  int chokeGroup{-1};        // regions sharing a group cut each other off
                             // (Hydrogen muteGroup, SF2 exclusiveClass,
                             //  DLS/gig KeyGroup); -1 = none
  bool releaseTrigger{false}; // triggered on note-off instead of note-on
  int rrIndex{-1};           // alternation index within its zone (gig
                             // round-robin/random dimensions); -1 = none
  int selectionAlgo{0};      // how same-zone alternatives are picked:
                             // 0 = all/velocity, 1 = round-robin, 2 = random

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

// The process customData is either a plain file path, or
// "<path>|<instrument index>" to select a specific instrument (gig/dls) or
// preset (sf2) of a multi-instrument file.
struct ParsedInstrumentPath
{
  QString file;
  int instrument{0};
};
ParsedInstrumentPath parseInstrumentPath(const QString& data);

// Lists the names of all instruments (gig/dls) or presets (sf2) of a file,
// without loading any sample data. Returns an empty list on error.
std::vector<std::string> listInstruments(const QString& filePath);

// Human-readable format family of a sample file, determined by content
// sniffing with the extension as fallback: "GIG", "DLS", "SF2" or "Drumkit".
QString formatName(const QString& filePath);

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
    const std::shared_ptr<std::atomic<bool>>& cancelled);

}
