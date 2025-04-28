#include "Drumkit.hpp"

#include <Media/AudioDecoder.hpp>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

namespace Deuterium
{

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
  auto folder = QFileInfo{file.fileName()}.dir().absolutePath();

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

  while(!instrumentNode.isNull())
  {
    QDomElement instrumentElem = instrumentNode.toElement();
    if(instrumentElem.tagName() == "instrument")
    {
      Instrument instrument;

      auto name = instrumentElem.firstChildElement("name").text();
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
      if(!midi)
        continue;

      instrument.id = instrumentElem.firstChildElement("id").text().toInt();
      instrument.name = name.toStdString();
      instrument.volume = instrumentElem.firstChildElement("volume").text().toFloat();
      instrument.isMuted
          = (instrumentElem.firstChildElement("isMuted").text() == "true");
      instrument.pan_L = instrumentElem.firstChildElement("pan_L").text().toFloat();
      instrument.pan_R = instrumentElem.firstChildElement("pan_R").text().toFloat();
      instrument.randomPitchFactor
          = instrumentElem.firstChildElement("randomPitchFactor").text().toFloat();
      instrument.filterActive
          = (instrumentElem.firstChildElement("filterActive").text() == "true");
      instrument.filterCutoff
          = instrumentElem.firstChildElement("filterCutoff").text().toFloat();
      instrument.filterResonance
          = instrumentElem.firstChildElement("filterResonance").text().toFloat();
      instrument.Attack = instrumentElem.firstChildElement("Attack").text().toFloat();
      instrument.Decay = instrumentElem.firstChildElement("Decay").text().toFloat();
      instrument.Sustain = instrumentElem.firstChildElement("Sustain").text().toFloat();
      instrument.Release = instrumentElem.firstChildElement("Release").text().toFloat();

      // Parse layers
      QDomNode layerNode = instrumentElem.firstChildElement("layer");
      while(!layerNode.isNull())
      {
        QDomElement layerElem = layerNode.toElement();
        if(layerElem.tagName() == "layer")
        {
          // FIXME within archive?
          Layer layer;
          auto filename = layerElem.firstChildElement("filename").text();

          auto dec = Media::AudioDecoder::decode_synchronous(
              folder + QDir::separator() + filename, 48000); // FIXME
          if(!dec)
            continue;

          layer.filename = filename.toStdString();
          layer.data = std::move(dec->second);
          layer.min = layerElem.firstChildElement("min").text().toFloat();
          layer.max = layerElem.firstChildElement("max").text().toFloat();
          layer.gain = layerElem.firstChildElement("gain").text().toFloat();
          layer.pitch = layerElem.firstChildElement("pitch").text().toFloat();

          instrument.layers.push_back(layer);
        }
        layerNode = layerNode.nextSibling();
      }

      drumkit.instruments.push_back(instrument);
    }
    instrumentNode = instrumentNode.nextSibling();
  }

  return drumkit_p;
}
}
