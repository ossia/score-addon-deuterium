#pragma once
#include <Process/Process.hpp>

#include <Control/DefaultEffectItem.hpp>
#include <Effect/EffectFactory.hpp>

#include <Deuterium/GigSampler/GigLoader.hpp>
#include <Deuterium/GigSampler/ProcessMetadata.hpp>

#include <score_addon_deuterium_export.h>

#include <atomic>
#include <memory>
#include <verdigris>

namespace Deuterium::Gig
{
class ProcessModel;
class SCORE_ADDON_DEUTERIUM_EXPORT ProcessModel final : public Process::ProcessModel
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
  void mapExternalFiles(Process::ExternalFileMap& map) override;
  // Accepts a plain path or the "<path>|<instrument>" creation string
  void loadFile(const QString& data);
  void loadFile(const QString& path, int instrument);
  int instrument() const noexcept { return m_instrument; }
  void fileChanged() E_SIGNAL(SCORE_ADDON_DEUTERIUM_EXPORT, fileChanged)

  //! Live note trigger from the panel's keyboard / pads widget; routed to
  //! the execution component which plays it on the next buffer.
  void uiNoteTriggered(int note, int velocity, bool on)
      E_SIGNAL(SCORE_ADDON_DEUTERIUM_EXPORT, uiNoteTriggered, note, velocity, on)

  std::shared_ptr<GigFileInfo> gigInfo() const noexcept { return m_gigInfo; }

  //! Cancellation flag of the load currently in flight, if any. Starting
  //! another load sets it, which is how a switch away from a half-loaded
  //! instrument stops paying for it.
  std::shared_ptr<const std::atomic<bool>> currentLoadToken() const noexcept
  {
    return m_cancelToken;
  }

private:
  void wireInstrumentControl();
  void startAsyncLoad();
  //! A stored path (absolute, document-relative, <PROJECT>: or <LIBRARY>:)
  //! as a file the loader can open.
  QString resolvedPath(const QString& stored) const;
  //! m_filePath as a document saves it: <PROJECT>: or <LIBRARY>: when the
  //! file lives under the document's folder or the user library.
  QString storedPath() const;

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
