#pragma once

// Custom control panel for the Deuterium sampler: a header with the loaded
// bank / instrument, and the 40 controls grouped in five tabs instead of the
// auto-generated 5-row grid.

#include <Process/Dataflow/ControlWidgets.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/Dataflow/PortType.hpp>
#include <Process/ProcessFactory.hpp>

#include <Control/Layout.hpp>
#include <Effect/EffectFactory.hpp>

#include <Process/Commands/SetControlValue.hpp>

#include <score/application/GUIApplicationContext.hpp>
#include <score/command/Dispatchers/CommandDispatcher.hpp>
#include <score/graphics/TextItem.hpp>
#include <score/graphics/layouts/GraphicsBoxLayout.hpp>
#include <score/graphics/layouts/GraphicsGridLayout.hpp>
#include <score/graphics/layouts/GraphicsTabLayout.hpp>
#include <score/graphics/widgets/QGraphicsCombo.hpp>
#include <score/model/Skin.hpp>

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

  // One control (port dot + widget + label) from the process's inlets,
  // through the inlet's own widget factory.
  void control(QGraphicsItem* parent, int samplerControl)
  {
    const int idx = 1 + samplerControl;
    if(idx >= std::ssize(inlets) || !inlets[idx])
      return;
    auto item = makePort(*inlets[idx]);
    if(item.container)
      item.container->setParentItem(parent);
  }

  // Float controls as knobs in a fixed-size cell (label above, port dot on
  // the left): denser and uniform. Everything else falls back to the
  // inlet's own widget.
  void knobOrControl(QGraphicsItem* parent, int samplerControl)
  {
    static constexpr qreal cell_w = 50., cell_h = 60.;
    const int idx = 1 + samplerControl;
    if(idx >= std::ssize(inlets) || !inlets[idx])
      return;
    auto* ctl = qobject_cast<Process::ControlInlet*>(inlets[idx]);
    if(!ctl)
      return control(parent, samplerControl);

    struct Range
    {
      float mn, mx, in;
      float getMin() const noexcept { return mn; }
      float getMax() const noexcept { return mx; }
      float getInit() const noexcept { return in; }
    };
    const auto& dom = ctl->domain().get();
    const Range range{
        dom.convert_min<float>(), dom.convert_max<float>(),
        ossia::convert<float>(ctl->init())};

    auto* cell = new score::EmptyRectItem{parent};
    cell->setRect({0., 0., cell_w, cell_h});

    QGraphicsItem* w{};
    if(dynamic_cast<Process::LogFloatSlider*>(ctl))
      w = WidgetFactory::LogFloatKnob::make_item(range, *ctl, doc, cell, &context);
    else if(dynamic_cast<Process::FloatSlider*>(ctl))
      w = WidgetFactory::FloatKnob::make_item(range, *ctl, doc, cell, &context);
    if(!w)
    {
      delete cell;
      return control(parent, samplerControl);
    }
    w->setParentItem(cell);
    w->setPos((cell_w - 35.) / 2., 14.);

    auto* lab = makeLabel(ctl->name().toStdString());
    lab->setParentItem(cell);
    const auto lw = lab->boundingRect().width();
    lab->setPos(std::max(0., (cell_w - lw) / 2.), 0.);

    if(auto* pf = portFactory.get(ctl->concreteKey()))
      if(auto* dot = pf->makePortItem(
             *ctl, static_cast<const Process::Context&>(doc), cell, &context))
        dot->setPos(0., 24.);
  }

  // A fixed-column grid: every control sits in a uniform, centred cell so
  // rows and columns align (the layout computes the cell from the largest
  // child).
  void grid(QGraphicsItem* parent, int columns, std::initializer_list<int> controls)
  {
    auto* g = start<score::GraphicsGridColumnsLayout>(parent, 2.);
    g->setColumns(columns);
    for(int c : controls)
      knobOrControl(g, c);
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
    build();
    // The user may be interacting with one of the widgets when a load
    // finishes: never tear the tree down at runtime, update it in place.
    connect(&model, &ProcessModel::fileChanged, this, [this] { refresh(); });
  }

private:
  void refresh()
  {
    if(m_instruments)
    {
      m_instruments->array = instrumentNames();
      const int idx = m_model.instrument();
      if(idx >= 0 && idx < m_instruments->array.size())
        m_instruments->setValue(idx);
    }
  }

  QStringList instrumentNames() const
  {
    QStringList l;
    if(auto gi = m_model.gigInfo())
      for(std::size_t i = 0; i < gi->instruments.size(); i++)
        l.push_back(
            gi->instruments[i].name.empty()
                ? QStringLiteral("Instrument %1").arg(i)
                : QString::fromStdString(gi->instruments[i].name));
    if(l.empty())
      l.push_back(QStringLiteral("-"));
    return l;
  }

  void build()
  {
    UiBuilder b{
        *this,
        m_model,
        m_ctx,
        m_ctx.app.interfaces<Process::PortFactoryList>(),
        m_model.inlets(),
        m_model.outlets()};

    auto& skin = score::Skin::instance();

    auto* main = b.start<score::GraphicsVBoxLayout>(nullptr, 4.);
    main->setBrush(skin.Background2.darker);

    // Header strip: MIDI port, file chooser, instrument, bank name
    {
      auto* header = b.start<score::GraphicsHBoxLayout>(main, 4.);
      header->setBrush(skin.Background2.darker300);
      if(!m_model.inlets().empty())
      {
        auto midi = b.makePort(*m_model.inlets().front());
        if(midi.container)
          midi.container->setParentItem(header);
      }
      b.control(header, Gig::File);

      // Instrument chooser: a combo listing the file's instruments, driving
      // the Instrument inlet (refreshed in place on load)
      if(auto* ctl = qobject_cast<Process::ControlInlet*>(
             m_model.inlets()[1 + Gig::Instrument]))
      {
        auto* cell = new score::EmptyRectItem{header};
        m_instruments = new score::QGraphicsCombo{instrumentNames(), cell};
        const int idx = m_model.instrument();
        if(idx >= 0 && idx < m_instruments->array.size())
          m_instruments->setValue(idx);
        cell->setRect(m_instruments->boundingRect());

        connect(
            m_instruments, &score::QGraphicsCombo::sliderMoved, this, [this, ctl] {
          m_instruments->moving = true;
          m_ctx.dispatcher.submit<Process::SetControlValue>(
              *ctl, m_instruments->value());
        });
        connect(
            m_instruments, &score::QGraphicsCombo::sliderReleased, this, [this, ctl] {
          m_ctx.dispatcher.submit<Process::SetControlValue>(
              *ctl, m_instruments->value());
          m_ctx.dispatcher.commit();
          m_instruments->moving = false;
        });
        if(auto* pf = b.portFactory.get(ctl->concreteKey()))
          if(auto* dot = pf->makePortItem(*ctl, m_ctx, cell, this))
            dot->setPos(0., 4.);
      }
    }

    auto* tabs = b.start<score::GraphicsTabLayout>(main, 3.);
    tabs->setBrush(skin.Background2.darker300);
    tabs->addTab(QStringLiteral(" Main "));
    tabs->addTab(QStringLiteral(" Sample "));
    tabs->addTab(QStringLiteral(" Filter "));
    tabs->addTab(QStringLiteral(" Amp "));
    tabs->addTab(QStringLiteral(" Mod "));

    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
      page->setBrush(skin.Background2.main);
      b.grid(page, 5, {Volume, Pan, Transpose, FineTune, BendRange});
      b.grid(page, 3, {Chromatic, ChromaticRoot, RoundRobin});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
      page->setBrush(skin.Background2.main);
      b.grid(page, 3, {StartOffset, Reverse, Lofi});
      b.grid(page, 3, {LoopMode, LoopXfade, VelToStart});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
      page->setBrush(skin.Background2.main);
      b.grid(page, 5, {FilterType, Cutoff, Resonance, FilterKeytrack, VelToCutoff});
      b.grid(
          page, 5,
          {FilterEnvAmount, FilterEnvAttack, FilterEnvDecay, FilterEnvSustain,
           FilterEnvRelease});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
      page->setBrush(skin.Background2.main);
      b.grid(page, 5, {EnvFromFile, Attack, Decay, Sustain, Release});
      b.grid(page, 5, {VelAmount, VelCurve, VelXfade, PitchEnvAmount, PitchEnvDecay});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
      page->setBrush(skin.Background2.main);
      b.grid(page, 4, {LfoDest, LfoRate, LfoDepth, LfoDelay});
      b.grid(page, 4, {VoiceMode, Glide, Polyphony});
    }

    b.finalizeLayout(this);
    fitChildrenRect();
  }

  const ProcessModel& m_model;
  const Process::Context& m_ctx;
  score::QGraphicsCombo* m_instruments{};
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
