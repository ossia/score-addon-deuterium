#pragma once
#include <Process/Process.hpp>

#include <Control/DefaultEffectItem.hpp>
#include <Effect/EffectFactory.hpp>

#include <score/widgets/PluginWindow.hpp>

#include <Deuterium/Drumkit.hpp>
#include <Deuterium/ProcessMetadata.hpp>

#include <memory>
#include <verdigris>

namespace Deuterium
{
class ProcessModel;
class ProcessModel final : public Process::ProcessModel
{
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Deuterium::ProcessModel)
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

  void loadDrumkit(const QString& path);
  void drumkitChanged() W_SIGNAL(drumkitChanged)

  std::shared_ptr<DrumkitInfo> drumkit() const noexcept { return m_drumkit; }

  std::unique_ptr<Process::Inlet> midi_in;
  std::unique_ptr<Process::Outlet> audio_out;

private:
  std::vector<Process::Preset> builtinPresets() const noexcept override;
  void loadPreset(const Process::Preset&) override;
  QString m_drumkitPath;
  std::shared_ptr<DrumkitInfo> m_drumkit;
};

}
