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
  // Decoded audio, shared between all regions that reference the same
  // source sample (null until phase 2 populated it)
  std::shared_ptr<ossia::audio_array> data;
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
  // SF2 sampleModes 3 / DLS release loops: loop while the note is held,
  // then play through the rest of the sample
  bool loopUntilRelease{false};
};

struct GigRegion
{
  uint8_t keyLow{0};
  uint8_t keyHigh{127};
  uint8_t velLow{0};
  uint8_t velHigh{127};

  // EG1 (amplitude)
  double eg1Delay{0.0};
  double eg1Attack{0.0};
  double eg1Hold{0.0};
  double eg1Decay{0.0};
  double eg1Sustain{1.0};
  double eg1Release{0.05};
  // dB-slope (EMU) time semantics: decay time is for a full 96 dB fall and
  // ends at the sustain level (SF2/DLS); false = time-to-sustain semantics
  // (gig, Hydrogen, user knobs)
  bool eg1DbSlope{false};
  // Hold scaling per key (SF2 keynumToVolEnvHold), timecents/key rel. key 60
  float keynumToHold{0.f};

  // Filter
  bool vcfEnabled{false};
  uint8_t vcfCutoff{127};
  uint8_t vcfResonance{0};
  // File-specified resonance in centibels (SF2 initialFilterQ / DLS filter Q);
  // < 0 = not set, use the vcfResonance byte. The engine applies the SF2
  // -3.01 dB convention and passband gain compensation for this path.
  float vcfQCb{-1.f};

  // Playback
  bool pitchTrack{true};
  double keyScale{1.0}; // semitones of pitch per key step (SF2 scaleTuning/100)
  int8_t pan{0};        // -64..63
  double sampleAttenuation{1.0};
  uint32_t sampleStartOffset{0};

  // Vol env decay scaling per key (SF2 keynumToVolEnvDecay, timecents/key
  // relative to key 60); 0 = none
  float keynumToDecay{0.f};

  // File-specified vibrato (SF2 vibLfo); toPitch in cents, 0 = none
  float vibLfoToPitch{0.f};
  float vibLfoFreq{4.f};   // Hz
  float vibLfoDelay{0.f};  // seconds

  // File-specified modLfo (SF2): tremolo / filter wobble / pitch
  float modLfoToPitch{0.f}; // cents, 0 = none
  float modLfoToFc{0.f};    // cents
  float modLfoToVol{0.f};   // centibels
  float modLfoFreq{4.f};    // Hz
  float modLfoDelay{0.f};   // seconds

  // File-specified modulation envelope (SF2 EG2) -> pitch / cutoff
  float modEnvToPitch{0.f}; // cents, 0 = none
  float modEnvToFc{0.f};    // cents
  float eg2Delay{0.f}, eg2Attack{0.f}, eg2Hold{0.f}, eg2Decay{0.f};
  float eg2Sustain{1.f}; // 0..1 fraction of the envelope peak
  float eg2Release{0.f};

  // Exact filter cutoff when the file specifies one (Hz); < 0 = use the
  // quantized vcfCutoff byte (gig / Hydrogen paths)
  float vcfCutoffHz{-1.f};

  // SF2 default velocity->cutoff modulator amount in cents (0 = disabled;
  // banks can override it, including to 0); applies for velocities >= 64
  float velToFcCents{0.f};

  // Instrument-zone key / velocity overrides (SF2 gens 46/47); -1 = none.
  // Zone matching and note-off use the played values, pitch and velocity
  // response use these.
  int forcedKey{-1};
  int forcedVelocity{-1};

  // Trailing frames cut off the sample end (SF2 endAddrsOffset, stored
  // positive); rescaled with the loop points when resampling
  uint32_t sampleEndOffset{0};

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

  // Display name of the sound, for formats that have one (Hydrogen drum
  // names); shown on the trigger pads
  std::string noteLabel;

  // Precomputed alternation grouping (regions sharing the exact same key and
  // velocity zone), so the audio thread never has to group; see
  // assignAlternationGroups()
  int altGroup{-1};
  uint8_t altIndex{0};
  uint8_t altCount{1};

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
