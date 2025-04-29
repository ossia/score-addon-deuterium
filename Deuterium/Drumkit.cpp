#include "Drumkit.hpp"

#include <Audio/Settings/Model.hpp>
#include <Media/AudioDecoder.hpp>

#include <score/tools/ThreadPool.hpp>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

namespace Deuterium
{
// fixme velocity layers
// casio mt500 no sound & mt800

std::shared_ptr<DrumkitInfo> parseDrumkit(const QString& filePath)
{
  QFile file(filePath);
  if(!file.open(QIODevice::ReadOnly))
  {
    qWarning() << "Failed to open file:" << filePath;
    return {};
  }

  QDomDocument doc;
  if(!doc.setContent(&file))
  {
    qWarning() << "Failed to parse XML content.";
    return {};
  }
  file.close();

  QDomElement root = doc.documentElement(); // <drumkit_info>
  if(root.tagName() != "drumkit_info")
  {
    qWarning() << "Unexpected root element:" << root.tagName();
    return {};
  }
  auto dir = QFileInfo{file.fileName()}.dir();
  auto folder = dir.absolutePath();

  std::shared_ptr<DrumkitInfo> drumkit_p = std::make_shared<DrumkitInfo>();
  auto& drumkit = *drumkit_p;

  // Read basic drumkit info
  drumkit.name = root.firstChildElement("name").text().toStdString();
  drumkit.author = root.firstChildElement("author").text().toStdString();
  drumkit.info = root.firstChildElement("info").text().toStdString();
  drumkit.license = root.firstChildElement("license").text().toStdString();

  // Parse instrument list
  QDomElement instrumentListElem = root.firstChildElement("instrumentList");
  QDomNode instrumentNode = instrumentListElem.firstChild();

  int base_midi_note = 36; // for instruments that do not specify it

  while(!instrumentNode.isNull())
  {
    QDomElement instrumentElem = instrumentNode.toElement();
    if(instrumentElem.tagName() == "instrument")
    {
      Instrument instrument;

      auto name = instrumentElem.firstChildElement("name").text();
      auto midiOutNote = instrumentElem.firstChildElement("midiOutNote");

      // <applyVelocity>true</applyVelocity>
      //      <sampleSelectionAlgo>VELOCITY</sampleSelectionAlgo>
      if(midiOutNote.isNull())
      {
        auto num = name;
        for(int i = 0; i < num.size(); i++)
        {
          if(num[i].isDigit())
            continue;
          num = num.mid(0, i);
          break;
        }
        bool midi{};
        instrument.midi_note = num.toInt(&midi);
        if(!midi || instrument.midi_note < 0 || instrument.midi_note > 127)
          instrument.midi_note = base_midi_note;
      }
      else
      {
        instrument.midi_note = midiOutNote.text().toInt();
      }
      base_midi_note = instrument.midi_note + 1;

      instrument.id = instrumentElem.firstChildElement("id").text().toInt();
      instrument.name = name.toStdString();
      instrument.volume = instrumentElem.firstChildElement("volume").text().toFloat();
      instrument.isMuted
          = (instrumentElem.firstChildElement("isMuted").text() == "true");
      instrument.pan_L = instrumentElem.firstChildElement("pan_L").text().toFloat();
      instrument.pan_R = instrumentElem.firstChildElement("pan_R").text().toFloat();
      instrument.randomPitchFactor
          = instrumentElem.firstChildElement("randomPitchFactor").text().toFloat();

      if(!instrumentElem.firstChildElement("filterActive").isNull())
      {
        instrument.filterActive
            = (instrumentElem.firstChildElement("filterActive").text() == "true");
        instrument.filterCutoff
            = instrumentElem.firstChildElement("filterCutoff").text().toFloat();
        instrument.filterResonance
            = instrumentElem.firstChildElement("filterResonance").text().toFloat();
      }

      if(!instrumentElem.firstChildElement("Attack").isNull())
      {
        instrument.Attack = instrumentElem.firstChildElement("Attack").text().toFloat();
        instrument.Decay = instrumentElem.firstChildElement("Decay").text().toFloat();
        instrument.Sustain
            = instrumentElem.firstChildElement("Sustain").text().toFloat();
        instrument.Release
            = instrumentElem.firstChildElement("Release").text().toFloat();
      }
      // Parse layers
      QDomNode layerNode = instrumentElem.firstChildElement("layer");
      QDomNode instrumentComponentNode
          = instrumentElem.firstChildElement("instrumentComponent");
      if(layerNode.isNull() && instrumentComponentNode.isNull())
      {
        // Oldest format
        Layer layer;
        auto filename = instrumentElem.firstChildElement("filename").text();
        if(!filename.isEmpty() && dir.exists(filename))
        {
          layer.filename = filename.toStdString();
          instrument.layers.push_back(layer);
        }
      }
      else if(!layerNode.isNull())
      {
        // Old format
        while(!layerNode.isNull())
        {
          QDomElement layerElem = layerNode.toElement();
          if(layerElem.tagName() == "layer")
          {
            // FIXME within archive?
            Layer layer;
            auto filename = layerElem.firstChildElement("filename").text();
            if(!filename.isEmpty() && dir.exists(filename))
            {
              layer.filename = filename.toStdString();
              layer.min = layerElem.firstChildElement("min").text().toFloat();
              layer.max = layerElem.firstChildElement("max").text().toFloat();
              layer.gain = layerElem.firstChildElement("gain").text().toFloat();
              layer.pitch = layerElem.firstChildElement("pitch").text().toFloat();

              instrument.layers.push_back(layer);
            }
          }
          layerNode = layerNode.nextSibling();
        }
      }
      else if(!instrumentComponentNode.isNull())
      {
        // Newer format
        layerNode = instrumentComponentNode.firstChildElement("layer");
        while(!layerNode.isNull())
        {
          QDomElement layerElem = layerNode.toElement();
          if(layerElem.tagName() == "layer")
          {
            // FIXME within archive?
            Layer layer;
            auto filename = layerElem.firstChildElement("filename").text();
            if(!filename.isEmpty() && dir.exists(filename))
            {
              layer.filename = filename.toStdString();
              layer.min = layerElem.firstChildElement("min").text().toFloat();
              layer.max = layerElem.firstChildElement("max").text().toFloat();
              layer.gain = layerElem.firstChildElement("gain").text().toFloat();
              layer.pitch = layerElem.firstChildElement("pitch").text().toFloat();

              instrument.layers.push_back(layer);
            }
          }
          layerNode = layerNode.nextSibling();
        }
      }

      if(!instrument.layers.empty())
      {
        drumkit.instruments.push_back(std::move(instrument));
      }
    }
    instrumentNode = instrumentNode.nextSibling();
  }

  // Load all the samples
  int sent = 0;
  int recv = 0;
  auto& tp = score::TaskPool::instance();
  auto rate = score::AppContext().settings<Audio::Settings::Model>().getRate();
  for(auto& inst : drumkit.instruments)
  {
    for(auto& layer : inst.layers)
    {
      QString filename
          = folder + QDir::separator() + QString::fromStdString(layer.filename);

      sent++;
      tp.post([filename, &layer, &recv, rate] {
        try
        {
          auto dec = Media::AudioDecoder::decode_synchronous(filename, rate); // FIXME
          layer.data = std::move(dec->second);
        }
        catch(...)
        {
        }

        recv++;
      });
    }
  }

  while(recv != sent)
    std::this_thread::yield();

  return drumkit_p;
}
}
