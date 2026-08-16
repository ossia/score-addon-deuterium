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
#include <Deuterium/GigSampler/KeyboardItem.hpp>
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
      // Non-float control: wrap its standard port item in a same-height cell,
      // top-aligned, so the labels of a grid row all sit on the same line.
      auto item = makePort(*ctl);
      if(!item.container)
      {
        delete cell;
        return;
      }
      const auto br = item.container->boundingRect();
      const double cw = std::max(cell_w, br.width() + 8.);
      cell->setRect({0., 0., cw, cell_h});
      item.container->setParentItem(cell);
      item.container->setPos(std::max(0., (cw - br.width()) / 2.), 0.);
      return;
    }
    w->setParentItem(cell);
    // Same widget y as DefaultControlLayouts::knob() so the knobs of a row
    // line up with the controls built through makePort
    w->setPos((cell_w - 35.) / 2., 6.);

    auto* lab = makeLabel(ctl->name().toStdString());
    lab->setParentItem(cell);
    const auto lw = lab->boundingRect().width();
    lab->setPos(std::max(0., (cell_w - lw) / 2.), 0.);

    if(auto* pf = portFactory.get(ctl->concreteKey()))
      if(auto* dot = pf->makePortItem(
             *ctl, static_cast<const Process::Context&>(doc), cell, &context))
        dot->setPos(0., 17.);
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
    refresh();
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
    if(m_instrumentIndex)
    {
      std::size_t count = 0;
      if(auto gi = m_model.gigInfo())
        count = gi->instruments.size();
      m_instrumentIndex->setText(
          count > 1 ? QStringLiteral("%1/%2").arg(m_model.instrument() + 1).arg(count)
                    : QString{});
    }
    updateKeyboard();
  }

  void updateKeyboard()
  {
    if(!m_keyboard)
      return;
    std::bitset<128> mapped;
    QHash<int, QString> names;
    int singleKey = 0, total = 0, rootTracksKey = 0;
    if(auto gi = m_model.gigInfo())
    {
      if(gi->selectedInstrument >= 0
         && gi->selectedInstrument < std::ssize(gi->instruments))
      {
        for(const auto& r : gi->instruments[gi->selectedInstrument].regions)
        {
          if(r.releaseTrigger || r.muted)
            continue;
          for(int k = r.keyLow; k <= (int)r.keyHigh && k < 128; k++)
            mapped.set(k);
          total++;
          if(r.keyLow == r.keyHigh)
          {
            singleKey++;
            // A chromatic multisample's sample roots follow the keys (one
            // region per key, root at or near it); drum regions keep a
            // fixed root far from most of their keys.
            const int delta = int(r.keyLow) - int(r.sample.midiUnityNote)
                              + (int)std::lround(r.pitchOffset);
            if(r.pitchTrack && std::abs(delta) <= 12)
              rootTracksKey++;
            if(!r.noteLabel.empty())
              names.insert(r.keyLow, QString::fromStdString(r.noteLabel));
          }
        }
      }
    }
    // Kits: mostly single-key regions, not chromatically rooted, and no
    // more elements than any real kit (library survey: the largest
    // Hydrogen kit has 82). Named elements are always a kit.
    const int mappedCount = (int)mapped.count();
    const bool chromatic = rootTracksKey * 10 >= singleKey * 9;
    const bool pads = !names.isEmpty()
                      || (total > 0 && singleKey * 2 > total && !chromatic
                          && mappedCount <= 82);
    m_keyboard->setMapped(mapped, pads, std::move(names));
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

    // Header strip: file chooser and instrument (the MIDI port itself is
    // already drawn by score's default node chrome)
    {
      auto* header = b.start<score::GraphicsHBoxLayout>(main, 4.);
      header->setBrush(skin.Background2.darker300);
      b.control(header, Gig::File);

      // Instrument chooser: a combo listing the file's instruments, driving
      // the Instrument inlet (refreshed in place on load)
      if(auto* ctl = qobject_cast<Process::ControlInlet*>(
             m_model.inlets()[1 + Gig::Instrument]))
      {
        auto* cell = new score::EmptyRectItem{header};
        auto* lab = b.makeLabel("Instrument");
        lab->setParentItem(cell);
        lab->setPos(10., 0.);
        // Bank position indicator ("3/17"): some banks name every
        // instrument identically. Hidden for single-instrument files.
        m_instrumentIndex
            = new score::SimpleTextItem{score::Skin::instance().Gray.main, cell};
        m_instrumentIndex->setFont(score::Skin::instance().Medium7Pt);
        m_instrumentIndex->setPos(12. + lab->boundingRect().width(), 1.);
        m_instruments = new score::QGraphicsCombo{instrumentNames(), cell};
        // Instrument names are long (bank presets, sampled articulations):
        // give the combo more room than the default slider width
        m_instruments->setRect({0., 0., 180., m_instruments->boundingRect().height()});
        m_instruments->setPos(10., 12.);
        const int idx = m_model.instrument();
        if(idx >= 0 && idx < m_instruments->array.size())
          m_instruments->setValue(idx);
        const auto cr = m_instruments->boundingRect();
        cell->setRect(
            {0., 0., 10. + std::max(cr.width(), lab->boundingRect().width()),
             12. + cr.height()});

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
            dot->setPos(0., 2.);
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
      b.grid(
          page, 6,
          {VelAmount, VelCurve, VelXfade, PitchEnvAmount, PitchEnvDecay,
           VelToPitchEnv});
    }
    {
      auto* page = b.start<score::GraphicsVBoxLayout>(tabs, 3.);
      page->setBrush(skin.Background2.main);
      b.grid(page, 4, {LfoDest, LfoRate, LfoDepth, LfoDelay});
      b.grid(page, 4, {VoiceMode, Glide, Polyphony});
    }

    // Trigger strip: compact keyboard / pads showing the mapped notes;
    // clicks play through the running execution, click height = velocity
    m_keyboard = new KeyboardItem{main};
    m_keyboard->noteOn = [this](int note, int velocity) {
      const_cast<ProcessModel&>(m_model).uiNoteTriggered(note, velocity, true);
    };
    m_keyboard->noteOff = [this](int note) {
      const_cast<ProcessModel&>(m_model).uiNoteTriggered(note, 0, false);
    };
    updateKeyboard();

    b.finalizeLayout(this);
    fitChildrenRect();

    // Now that the layout ran, the panel width is known: let the pads fill it
    m_keyboard->setAvailableWidth(main->boundingRect().width() - 8.);
    fitChildrenRect();
  }

  const ProcessModel& m_model;
  const Process::Context& m_ctx;
  score::QGraphicsCombo* m_instruments{};
  score::SimpleTextItem* m_instrumentIndex{};
  KeyboardItem* m_keyboard{};
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

  std::optional<double> recommendedHeight() const noexcept override { return 292.; }

  score::ResizeableItem* makeItem(
      const Process::ProcessModel& proc, const Process::Context& ctx,
      QGraphicsItem* parent) const override
  {
    return new LayerItem{static_cast<const ProcessModel&>(proc), ctx, parent};
  }
};
}
