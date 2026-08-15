#pragma once
#include <Process/Process.hpp>

#include <Control/DefaultEffectItem.hpp>
#include <Effect/EffectFactory.hpp>

#include <Deuterium/GigSampler/GigLoader.hpp>
#include <Deuterium/GigSampler/ProcessMetadata.hpp>

#include <atomic>
#include <memory>
#include <verdigris>

namespace Deuterium::Gig
{
class ProcessModel;
class ProcessModel final : public Process::ProcessModel
{
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Deuterium::Gig::ProcessModel)
  W_OBJECT(ProcessModel)
public:
  explicit ProcessModel(
      const TimeVal& duration, const QString& data, const Id<Process::ProcessModel>& id,
      QObject* parent);

  template <typename Impl>
  explicit ProcessModel(Impl& vis, QObject* parent)
      : Process::ProcessModel{vis, parent}
  {
    vis.writeTo(*this);
    wireInstrumentControl();
  }

  ~ProcessModel() override;

  QString effect() const noexcept override;
  // Accepts a plain path or the "<path>|<instrument>" creation string
  void loadFile(const QString& data);
  void loadFile(const QString& path, int instrument);
  int instrument() const noexcept { return m_instrument; }
  void fileChanged() W_SIGNAL(fileChanged)

  //! Live note trigger from the panel's keyboard / pads widget; routed to
  //! the execution component which plays it on the next buffer.
  void uiNoteTriggered(int note, int velocity, bool on)
      W_SIGNAL(uiNoteTriggered, note, velocity, on)

  std::shared_ptr<GigFileInfo> gigInfo() const noexcept { return m_gigInfo; }

  std::unique_ptr<Process::Inlet> midi_in;
  std::unique_ptr<Process::Outlet> audio_out;

private:
  void wireInstrumentControl();
  void startAsyncLoad();

  QString m_filePath;
  int m_instrument{};
  std::shared_ptr<GigFileInfo> m_gigInfo;
  std::shared_ptr<std::atomic<bool>> m_cancelToken;
};

}

// Declared here so that every translation unit - in particular unity builds
// merging a user of the serialization with ProcessModelSerialization.cpp -
// sees the explicit specializations before any implicit instantiation.
template <>
void DataStreamReader::read(const Deuterium::Gig::ProcessModel& proc);
template <>
void DataStreamWriter::write(Deuterium::Gig::ProcessModel& proc);
template <>
void JSONReader::read(const Deuterium::Gig::ProcessModel& proc);
template <>
void JSONWriter::write(Deuterium::Gig::ProcessModel& proc);
