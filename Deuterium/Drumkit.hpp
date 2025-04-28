#pragma once
#include <ossia/dataflow/nodes/media.hpp>

#include <string>
#include <vector>
namespace Deuterium
{

struct Layer
{
  std::string filename;
  ossia::audio_array data;
  double min = 0.0f;
  double max = 1.0f;
  double gain = 1.0f;
  double pitch = 0.0f;
};

struct Instrument
{
  std::string name;
  int id = 0;
  int midi_note = 0;
  double volume = 1.0f;
  double pan_L = 1.0f;
  double pan_R = 1.0f;

  double randomPitchFactor = 0.0f;

  double filterCutoff = 1.0f;
  double filterResonance = 0.0f;

  double Attack = 0.0f;
  double Decay = 0.0f;
  double Sustain = 1.0f;
  double Release = 1000.0f;

  bool isMuted = false;
  bool filterActive = false;
  std::vector<Layer> layers;
};

struct DrumkitInfo
{
  std::string name;
  std::string author;
  std::string info;
  std::string license;
  std::vector<Instrument> instruments;
};

std::shared_ptr<DrumkitInfo> parseDrumkit(const QString& filePath);
}
