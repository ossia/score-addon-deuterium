#pragma once

// Compact trigger keyboard for the sampler panel. Two modes, toggled by
// clicking the label in its top-right corner:
//  - Keys: a small piano covering the mapped range; mapped notes are lit.
//  - Pads: one pad per mapped note (drum kits).
// Clicking plays the note through the running execution; the vertical
// position inside a key / pad sets the velocity (lower = harder).

#include <score/model/Skin.hpp>

#include <QGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>

#include <QHash>

#include <bitset>
#include <functional>
#include <vector>

namespace Deuterium::Gig
{
class KeyboardItem final : public QGraphicsItem
{
public:
  enum class Mode
  {
    Keys,
    Pads
  };

  std::function<void(int note, int velocity)> noteOn;
  std::function<void(int note)> noteOff;

  explicit KeyboardItem(QGraphicsItem* parent)
      : QGraphicsItem{parent}
  {
    setAcceptedMouseButtons(Qt::LeftButton);
  }

  ~KeyboardItem() { releaseCurrent(); }

  void setMapped(
      const std::bitset<128>& mapped, bool preferPads,
      QHash<int, QString> names = {})
  {
    prepareGeometryChange();
    m_mapped = mapped;
    m_names = std::move(names);
    m_pads.clear();
    int lo = 128, hi = -1;
    for(int i = 0; i < 128; i++)
    {
      if(mapped[i])
      {
        lo = std::min(lo, i);
        hi = std::max(hi, i);
        m_pads.push_back(i);
      }
    }
    if(hi < 0)
    {
      lo = 48;
      hi = 72;
    }
    m_rawLo = lo;
    m_rawHi = hi;
    computeWindow();
    m_mode = preferPads && !m_pads.empty() ? Mode::Pads : Mode::Keys;
    update();
  }

  QRectF boundingRect() const override
  {
    return {0., 0., width(), header_h + bodyHeight()};
  }

  // Panel width the pads may occupy: rows wrap at this width and the pad
  // size stretches so each full row spans it exactly
  void setAvailableWidth(double w)
  {
    prepareGeometryChange();
    m_availW = std::max(120., w);
    computeWindow();
    update();
  }

private:
  // Shown octave window: whole octaves fitting the available width. A range
  // wider than that is windowed around middle C (or the range's centre), so
  // full-range GM banks show the musical middle instead of the lowest keys.
  void computeWindow()
  {
    m_lo = (m_rawLo / 12) * 12;
    m_hi = std::min(127, ((m_rawHi / 12) + 1) * 12);
    const int maxOct = std::clamp((int)(m_availW / key_w) / 7, 2, 10);
    if(m_hi - m_lo > maxOct * 12)
    {
      const int c = (m_rawLo <= 60 && 60 <= m_rawHi) ? 60 : (m_rawLo + m_rawHi) / 2;
      int wlo = ((c - maxOct * 6) / 12) * 12;
      wlo = std::clamp(wlo, m_lo, m_hi - maxOct * 12);
      m_lo = std::max(0, wlo);
      m_hi = m_lo + maxOct * 12;
    }
  }

  static constexpr double header_h = 12.;
  static constexpr double key_w = 8., key_h = 51., black_h = 30., label_h = 8.;
  static constexpr double pad_gap = 2.;
  // The pads area never grows below this: rows and pad size are chosen to
  // fit, so nothing is drawn outside the widget
  static constexpr double body_budget = 70.;

  double padTarget() const noexcept { return m_names.isEmpty() ? 26. : 52.; }

  // Row/size layout maximizing pad readability inside availW x body_budget
  struct PadGrid
  {
    int perRow{1}, rows{1};
    double w{26.}, h{22.};
  };
  PadGrid padGrid() const noexcept
  {
    PadGrid best;
    const int n = std::max<int>(1, (int)m_pads.size());
    double bestScore = -1.;
    for(int rows = 1; rows <= 4; rows++)
    {
      const int perRow = (n + rows - 1) / rows;
      const double w = std::clamp(
          (m_availW - (perRow + 1) * pad_gap) / perRow, 8., padTarget() * 1.6);
      const double h
          = std::clamp((body_budget - (rows + 1) * pad_gap) / rows, 8., 32.);
      const double score = std::min(w, h * 2.); // favor readable widths
      if(score > bestScore)
      {
        bestScore = score;
        best = {perRow, rows, w, h};
      }
    }
    return best;
  }

  static bool isBlack(int note) noexcept
  {
    switch(note % 12)
    {
      case 1:
      case 3:
      case 6:
      case 8:
      case 10:
        return true;
      default:
        return false;
    }
  }

  static QString noteName(int n)
  {
    static constexpr const char* names[12]
        = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return QStringLiteral("%1%2").arg(names[n % 12]).arg(n / 12 - 1);
  }

  int whiteCount() const noexcept
  {
    int c = 0;
    for(int n = m_lo; n <= m_hi; n++)
      c += !isBlack(n);
    return c;
  }

  double keysWidth() const noexcept { return whiteCount() * key_w; }

  double width() const noexcept
  {
    if(m_mode == Mode::Pads)
      return m_availW;
    return keysWidth();
  }

  double bodyHeight() const noexcept
  {
    if(m_mode == Mode::Pads)
    {
      const auto g = padGrid();
      return g.rows * (g.h + pad_gap) + pad_gap;
    }
    return key_h + label_h;
  }

  // The white-key rect of a note, or the black-key rect for sharps
  QRectF keyRect(int note) const noexcept
  {
    int white = 0;
    for(int n = m_lo; n < note; n++)
      white += !isBlack(n);
    if(!isBlack(note))
      return {white * key_w, header_h, key_w, key_h};
    // A black key sits across the boundary of the previous white key
    return {white * key_w - key_w * 0.3, header_h, key_w * 0.6, black_h};
  }

  QRectF padRect(std::size_t i) const noexcept
  {
    const auto g = padGrid();
    const int col = int(i % g.perRow), row = int(i / g.perRow);
    return {
        pad_gap + col * (g.w + pad_gap), header_h + pad_gap + row * (g.h + pad_gap),
        g.w, g.h};
  }

  // Flush-left mode selector, styled like the Enum widgets (clickable text)
  QRectF keysToggleRect() const noexcept { return {0., 0., 30., header_h}; }
  QRectF padsToggleRect() const noexcept { return {32., 0., 30., header_h}; }

  void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override
  {
    auto& skin = score::Skin::instance();
    p->setRenderHint(QPainter::Antialiasing, false);

    // Header: flush-left mode selector styled like the Enum widgets
    p->setFont(skin.MonoFontSmall);
    p->setPen(m_mode == Mode::Keys ? skin.Base4.main.pen1 : skin.Gray.main.pen1);
    p->drawText(
        keysToggleRect(), QStringLiteral("keys"),
        QTextOption(Qt::AlignLeft | Qt::AlignVCenter));
    p->setPen(m_mode == Mode::Pads ? skin.Base4.main.pen1 : skin.Gray.main.pen1);
    p->drawText(
        padsToggleRect(), QStringLiteral("pads"),
        QTextOption(Qt::AlignLeft | Qt::AlignVCenter));

    if(m_mode == Mode::Pads)
    {
      const auto grid = padGrid();
      const bool labels = grid.w >= 18.;
      // The corner note number needs vertical room next to the name
      const bool cornerNumbers = grid.h >= 20.;
      const QFontMetricsF fm{skin.Medium7Pt};
      for(std::size_t i = 0; i < m_pads.size(); i++)
      {
        const int note = m_pads[i];
        const auto r = padRect(i);
        p->setPen(skin.NoPen);
        p->setBrush(
            note == m_pressed ? skin.Base4.main.brush : skin.Emphasis2.main.brush);
        p->drawRoundedRect(r, 2., 2.);
        if(!labels)
          continue;
        p->setFont(skin.Medium7Pt);
        p->setPen(
            note == m_pressed ? skin.Background2.darker300.pen1
                              : skin.Base4.lighter180.pen1);
        if(const auto it = m_names.constFind(note); it != m_names.constEnd())
        {
          // The sound's name front and centre, the note number small in the
          // top-left corner when the pad is tall enough
          p->drawText(
              r.adjusted(1., cornerNumbers ? 6. : 0., -1., 0.),
              fm.elidedText(*it, Qt::ElideRight, r.width() - 2.),
              QTextOption(Qt::AlignCenter));
          if(cornerNumbers)
          {
            if(note != m_pressed)
              p->setPen(skin.Gray.main.pen1);
            p->drawText(
                QRectF{r.x() + 2., r.y() + 1., r.width() - 3., 8.}, noteName(note),
                QTextOption(Qt::AlignLeft));
          }
        }
        else
        {
          p->drawText(r, noteName(note), QTextOption(Qt::AlignCenter));
        }
      }
      return;
    }

    // White keys: warm ivory when mapped (same family as the knob readouts),
    // knob-body gray when unmapped, accent orange when pressed
    for(int n = m_lo; n <= m_hi; n++)
    {
      if(isBlack(n))
        continue;
      const auto r = keyRect(n);
      QBrush fill
          = m_mapped[n] ? skin.HalfLight.main.brush : skin.Emphasis2.main.brush;
      if(n == m_pressed)
        fill = skin.Base4.main.brush;
      p->setPen(skin.Background2.darker300.pen1);
      p->setBrush(fill);
      p->drawRect(r);
    }
    // Black keys on top
    for(int n = m_lo; n <= m_hi; n++)
    {
      if(!isBlack(n))
        continue;
      const auto r = keyRect(n);
      QBrush fill = m_mapped[n] ? skin.Background2.darker300.brush
                                : skin.Emphasis2.darker.brush;
      if(n == m_pressed)
        fill = skin.Base4.main.brush;
      p->setPen(skin.Background2.darker300.pen1);
      p->setBrush(fill);
      p->drawRect(r);
    }
    // Octave labels in their own row below the keys, aligned on the Cs
    QFont tiny = skin.Medium7Pt;
    tiny.setPointSizeF(std::max(4., tiny.pointSizeF() - 2.));
    p->setFont(tiny);
    p->setPen(skin.Base4.lighter180.pen1);
    for(int n = m_lo; n <= m_hi; n += 12)
    {
      const auto r = keyRect(n);
      p->drawText(
          QRectF{r.x(), header_h + key_h + 1., key_w * 3., label_h - 1.},
          noteName(n), QTextOption(Qt::AlignLeft));
    }
  }

  int noteAt(QPointF pos, int& velocity) const noexcept
  {
    velocity = 100;
    if(m_mode == Mode::Pads)
    {
      for(std::size_t i = 0; i < m_pads.size(); i++)
      {
        const auto r = padRect(i);
        if(r.contains(pos))
        {
          velocity = velocityFor(pos.y(), r.top(), r.height());
          return m_pads[i];
        }
      }
      return -1;
    }
    // Black keys take precedence over the whites under them
    for(int n = m_lo; n <= m_hi; n++)
      if(isBlack(n) && keyRect(n).contains(pos))
      {
        velocity = velocityFor(pos.y(), header_h, black_h);
        return n;
      }
    for(int n = m_lo; n <= m_hi; n++)
      if(!isBlack(n) && keyRect(n).contains(pos))
      {
        velocity = velocityFor(pos.y(), header_h, key_h);
        return n;
      }
    return -1;
  }

  static int velocityFor(double y, double top, double h) noexcept
  {
    return std::clamp((int)std::lround(1. + 126. * (y - top) / h), 1, 127);
  }

  void pressNote(int note, int velocity)
  {
    if(note == m_pressed)
      return;
    releaseCurrent();
    if(note >= 0 && noteOn)
    {
      noteOn(note, velocity);
      m_pressed = note;
      update();
    }
  }

  void releaseCurrent()
  {
    if(m_pressed >= 0)
    {
      if(noteOff)
        noteOff(m_pressed);
      m_pressed = -1;
      update();
    }
  }

  void mousePressEvent(QGraphicsSceneMouseEvent* ev) override
  {
    if(ev->pos().y() < header_h)
    {
      const Mode m = padsToggleRect().contains(ev->pos())   ? Mode::Pads
                     : keysToggleRect().contains(ev->pos()) ? Mode::Keys
                                                            : m_mode;
      if(m != m_mode)
      {
        prepareGeometryChange();
        m_mode = m;
        update();
      }
      ev->accept();
      return;
    }
    int vel = 100;
    pressNote(noteAt(ev->pos(), vel), vel);
    ev->accept();
  }

  void mouseMoveEvent(QGraphicsSceneMouseEvent* ev) override
  {
    int vel = 100;
    const int n = noteAt(ev->pos(), vel);
    if(n != m_pressed)
      pressNote(n, vel);
    ev->accept();
  }

  void mouseReleaseEvent(QGraphicsSceneMouseEvent* ev) override
  {
    releaseCurrent();
    ev->accept();
  }

  std::bitset<128> m_mapped;
  QHash<int, QString> m_names;
  std::vector<int> m_pads;
  double m_availW{480.};
  int m_rawLo{48}, m_rawHi{72};
  int m_lo{48}, m_hi{72};
  int m_pressed{-1};
  Mode m_mode{Mode::Keys};
};
}
