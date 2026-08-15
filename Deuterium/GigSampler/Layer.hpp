#pragma once

// Custom control panel for the Deuterium sampler: a header with the loaded
// bank / instrument, and the 40 controls grouped in five tabs instead of the
// auto-generated 5-row grid.

#include <Process/Dataflow/PortFactory.hpp>
#include <Process/ProcessFactory.hpp>

#include <Control/Layout.hpp>
#include <Effect/EffectFactory.hpp>

#include <score/graphics/TextItem.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>

#include <QFileInfo>

#include <Deuterium/GigSampler/Controls.hpp>
#include <Deuterium/GigSampler/GigLoader.hpp>
#include <Deuterium/GigSampler/ProcessModel.hpp>

namespace Deuterium::Gig
{
namespace
{
struct UiBuilder final : Process::LayoutBuilderBase
{
  // Creates a sub-layout, parents it, and records it for finalizeLayout()'s
  // bottom-up sizing pass (pre-order: parent before children).
  template <typename L>
  L* start(QGraphicsItem* parent, qreal padding = 4.)
  {
    auto* l = new L{parent};
    l->setPadding(padding);
    createdLayouts.push_back(l);
    return l;
  }

  // One control (port dot + widget + label) from the process's inlets.
  void control(QGraphicsItem* parent, int samplerControl)
  {
    const int idx = 1 + samplerControl;
    if(idx >= std::ssize(inlets) || !inlets[idx])
      return;
    auto item = makePort(*inlets[idx]);
    if(item.container)
      item.container->setParentItem(parent);
  }

  void row(QGraphicsItem* parent, std::initializer_list<int> controls)
  {
    auto* r = start<score::GraphicsHBoxLayout>(parent);
    for(int c : controls)
      control(r, c);
  }
};
}

class LayerItem final : public score::EmptyRectItem
{
public:
  LayerItem(
      const ProcessModel& model, const Process::Context& ctx, QGraphicsItem* parent)
      : score::EmptyRectItem{parent}
      , m_model{model}
      , m_ctx{ctx}
  {
    rebuild();
    connect(&model, &ProcessModel::fileChanged, this, [this] { rebuild(); });
  }

private:
  QString title() const
  {
    const auto& path = m_model.effect();
    if(path.isEmpty())
      return QStringLiteral("No file loaded");

    QString t = QFileInfo{path}.completeBaseName();
    if(auto gi = m_model.gigInfo())
    {
      const int idx = gi->selectedInstrument;
      if(idx >= 0 && idx < std::ssize(gi->instruments)
         && !gi->instruments[idx].name.empty())
      {
        t += QStringLiteral(" — ");
        t += QString::fromStdString(gi->instruments[idx].name);
      }
    }
    return t;
  }

  void rebuild()
  {
    // The layouts are one-shot (the tab layout in particular): build the
    // whole tree from scratch.
    const auto items = childItems();
    for(auto* item : items)
      delete item;

    UiBuilder b{
        *this,
        m_model,
        m_ctx,
        m_ctx.app.interfaces<Process::PortFactoryList>(),
        m_model.inlets(),
        m_model.outlets()};

    auto* main = b.start<score::GraphicsVBoxLayout>(nullptr);

    // Header: MIDI port + bank / instrument name
    {
      auto* header = b.start<score::GraphicsHBoxLayout>(main);
      if(!m_model.inlets().empty())
      {
        auto midi = b.makePort(*m_model.inlets().front());
        if(midi.container)
          midi.container->setParentItem(header);
      }
      auto* t = b.makeLabel(title().toStdString());
      t->setParentItem(header);
    }

    auto* tabs = b.start<score::GraphicsTabLayout>(main);
    tabs->addTab(QStringLiteral("Main"));
    tabs->addTab(QStringLiteral("Sample"));
    tabs->addTab(QStringLiteral("Filter"));
    tabs->addTab(QStringLiteral("Amp"));
    tabs->addTab(QStringLiteral("Mod"));

    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs);
      b.row(page, {Volume, Pan, Transpose, FineTune, BendRange});
      b.row(page, {Chromatic, ChromaticRoot, RoundRobin});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs);
      b.row(page, {StartOffset, Reverse, Lofi});
      b.row(page, {LoopMode, LoopXfade, VelToStart});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs);
      b.row(page, {FilterType, Cutoff, Resonance, FilterKeytrack});
      b.row(page, {FilterEnvAmount, VelToCutoff});
      b.row(page, {FilterEnvAttack, FilterEnvDecay, FilterEnvSustain, FilterEnvRelease});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs);
      b.row(page, {Attack, Decay, Sustain, Release});
      b.row(page, {VelAmount, VelCurve, VelXfade});
      b.row(page, {PitchEnvAmount, PitchEnvDecay});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs);
      b.row(page, {LfoDest, LfoRate, LfoDepth, LfoDelay});
      b.row(page, {VoiceMode, Glide, Polyphony});
    }

    b.finalizeLayout(this);
    fitChildrenRect();
  }

  const ProcessModel& m_model;
  const Process::Context& m_ctx;
};

class LayerFactory final : public Process::EffectLayerFactory_Base
{
public:
  ~LayerFactory() override = default;

private:
  UuidKey<Process::ProcessModel> concreteKey() const noexcept override
  {
    return Metadata<ConcreteKey_k, ProcessModel>::get();
  }

  bool matches(const UuidKey<Process::ProcessModel>& p) const override
  {
    return p == Metadata<ConcreteKey_k, ProcessModel>::get();
  }

  std::optional<double> recommendedHeight() const noexcept override { return 220.; }

  score::ResizeableItem* makeItem(
      const Process::ProcessModel& proc, const Process::Context& ctx,
      QGraphicsItem* parent) const override
  {
    return new LayerItem{static_cast<const ProcessModel&>(proc), ctx, parent};
  }
};
}
