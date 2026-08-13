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
  }

  ~ProcessModel() override;

  QString effect() const noexcept override;
  void loadFile(const QString& path);
  void fileChanged() W_SIGNAL(fileChanged)

  std::shared_ptr<GigFileInfo> gigInfo() const noexcept { return m_gigInfo; }

  std::unique_ptr<Process::Inlet> midi_in;
  std::unique_ptr<Process::Outlet> audio_out;

private:
  void startAsyncSampleLoad();

  QString m_filePath;
  std::shared_ptr<GigFileInfo> m_gigInfo;
  std::shared_ptr<std::atomic<bool>> m_cancelToken;
};

}
