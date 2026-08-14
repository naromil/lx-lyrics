/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "renderer/controlbar.h"

#include <QCursor>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVariantAnimation>

#include <cmath>
#include <numbers>

#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"

namespace {

// Reference ControlBar.vue: .btns { min-height: 38px } with .btn { padding:
// 0 10px } laid out full-width over a rgba(0, 0, 0, .7) strip; the strip's top
// corners follow the window radius (border-top-left/right-radius: 4px).
constexpr int kBarHeight = 38;
constexpr int kButtonPadding = 10; // reference .btn horizontal padding
constexpr int kIconSize = 16;
constexpr int kBarCornerRadius = 4; // reference border-top-left/right-radius

constexpr int kFontStep = 2;
constexpr int kFontSizeMin = 10;
constexpr int kFontSizeMax = 80;
constexpr int kOpacityStepClick = 10;      // left-click steps by ten
constexpr int kOpacityStepContextMenu = 2; // right-click steps by two
constexpr int kOpacityMin = 6;
constexpr int kOpacityMax = 100;

// Hover reveal durations: reference animate.less enter fadeIn .3s, leave
// fadeOut .5s (the #main:hover CSS transition is .4s; the .less override wins).
constexpr int kBarFadeInMs = 300;
constexpr int kBarFadeOutMs = 500;

const QColor kBarFill(0, 0, 0, 178);          // rgba(0, 0, 0, 0.7)
const QColor kIconColor(255, 255, 255, 235);  // white/light-gray strokes
const QColor kCheckedFill(255, 255, 255, 51); // rgba(255, 255, 255, 0.2)

const QString kKeyIsLock = QStringLiteral("desktopLyric.isLock");
const QString kKeyAlwaysOnTop = QStringLiteral("desktopLyric.isAlwaysOnTop");
const QString kKeyZoomActive = QStringLiteral("desktopLyric.style.isZoomActiveLrc");
const QString kKeyFontSize = QStringLiteral("desktopLyric.style.fontSize");
const QString kKeyOpacity = QStringLiteral("desktopLyric.style.opacity");

} // namespace

// Icon button with a fully custom paintEvent: draws a subtle checked backdrop
// and then the requested glyph with QPainter. Checkable buttons pick between
// two glyphs so the icon mirrors the reference ControlBar.vue (e.g. the "zoom
// off" glyph shows while zoom is active). Hovering dims the glyph to 0.7
// opacity (reference .btn:hover { opacity: .7 }).
class ControlBar::IconButton : public QToolButton {
public:
  IconButton(Icon iconOn, Icon iconOff, ControlBar* bar)
    : QToolButton(bar)
    , m_iconOn(iconOn)
    , m_iconOff(iconOff)
    , m_bar(bar)
  {
    // Full strip height with reference .btn padding: 0 10px — the glyph
    // stays centered, so the padding is the 10px around it.
    setFixedHeight(kBarHeight);
    setMinimumWidth(kIconSize + 2 * kButtonPadding);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAutoRaise(true);
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF iconRect =
      QRectF((width() - kIconSize) / 2.0, (height() - kIconSize) / 2.0, kIconSize, kIconSize);

    // Active state: a subtle halo behind the swapped glyph. The reference
    // marks active purely by the glyph swap; the halo keeps it readable
    // against the dark strip.
    if (isChecked()) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(kCheckedFill);
      painter.drawEllipse(iconRect.adjusted(-4.0, -4.0, 4.0, 4.0));
    }

    // Reference .btn:hover { opacity: .7 } — the glyph dims; the bar's own
    // opacity factor (hover reveal) compounds through the painter stack.
    if (underMouse())
      painter.setOpacity(0.7);

    m_bar->paintIcon(painter, isChecked() ? m_iconOn : m_iconOff, iconRect);
  }

private:
  Icon m_iconOn;
  Icon m_iconOff;
  ControlBar* m_bar;
};

ControlBar::ControlBar(DesktopLyricConfig& config, TranslationManager& i18n, QWidget* parent)
  : QWidget(parent)
  , m_config(config)
  , m_i18n(i18n)
{
  setFixedHeight(kBarHeight);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setAttribute(Qt::WA_TranslucentBackground); // the rounded-top strip shows through

  // Hover reveal (reference .control-bar opacity 0 / #main:hover opacity 1):
  // a graphics effect fades the whole strip — background and buttons — as
  // one unit, exactly like the CSS opacity transition. The reveal ceiling is
  // m_revealMaxOpacity (default 1.0 undimmed; LyricWindow pushes its body
  // dimming kBodyOpacity into the bar, since the window's container effect
  // now applies only the fade). The window's container fade (LyricWindow)
  // still compounds with this multiplier.
  m_hoverEffect = new QGraphicsOpacityEffect(this);
  m_hoverEffect->setOpacity(0.0);
  setGraphicsEffect(m_hoverEffect);

  m_hoverAnim = new QVariantAnimation(this);
  connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    m_hoverFactor = value.toDouble();
    m_hoverEffect->setOpacity(m_hoverFactor);
  });

  // Full-width strip: no margins, no gaps — each button carries its own
  // 10px horizontal padding (reference .btns row + .btn padding: 0 10px).
  m_layout = new QHBoxLayout(this);
  m_layout->setContentsMargins(0, 0, 0, 0);
  m_layout->setSpacing(0);

  // Reference order (ControlBar.vue): close, lock, font+, font-, opacity+,
  // opacity-, zoom, always-on-top; the settings gear is appended last.
  auto* closeButton = addIconButton(Icon::Close, QStringLiteral("desktop_lyric__close"));
  closeButton->setObjectName(QStringLiteral("closeButton"));
  // Close only requests the animated close: LyricWindow fades the content
  // out (300 ms) and quits the app afterwards (closeAnimationFinished). The
  // objectName lets the controller tests find the button.
  connect(closeButton, &QToolButton::clicked, this, &ControlBar::closeRequested);

  auto* lockButton = addIconButton(Icon::Lock, QStringLiteral("desktop_lyric__lock"));
  connect(lockButton, &QToolButton::clicked, this, [this] {
    m_config.set(kKeyIsLock, true);
  });

  auto* fontIncreaseButton =
    addIconButton(Icon::FontIncrease, QStringLiteral("desktop_lyric__font_increase"));
  connect(fontIncreaseButton, &QToolButton::clicked, this, [this] {
    changeFontSize(+kFontStep);
  });

  auto* fontDecreaseButton =
    addIconButton(Icon::FontDecrease, QStringLiteral("desktop_lyric__font_decrease"));
  connect(fontDecreaseButton, &QToolButton::clicked, this, [this] {
    changeFontSize(-kFontStep);
  });

  // Opacity: left-click steps by ten, right-click (context menu) by two —
  // reference click +10/-10, contextmenu +2/-2, both clamped 6..100.
  auto* opacityIncreaseButton =
    addIconButton(Icon::OpacityIncrease, QStringLiteral("desktop_lyric__opacity_increase"));
  connect(opacityIncreaseButton, &QToolButton::clicked, this, [this] {
    changeOpacity(+kOpacityStepClick);
  });
  opacityIncreaseButton->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(opacityIncreaseButton, &QWidget::customContextMenuRequested, this, [this] {
    changeOpacity(+kOpacityStepContextMenu);
  });

  auto* opacityDecreaseButton =
    addIconButton(Icon::OpacityDecrease, QStringLiteral("desktop_lyric__opacity_decrease"));
  connect(opacityDecreaseButton, &QToolButton::clicked, this, [this] {
    changeOpacity(-kOpacityStepClick);
  });
  opacityDecreaseButton->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(opacityDecreaseButton, &QWidget::customContextMenuRequested, this, [this] {
    changeOpacity(-kOpacityStepContextMenu);
  });

  m_zoomButton =
    addIconButton(Icon::ZoomOff, Icon::Zoom, QStringLiteral("desktop_lyric__lrc_active_zoom_on"),
                  QStringLiteral("desktop_lyric__lrc_active_zoom_off"));
  m_zoomButton->setCheckable(true);
  connect(m_zoomButton, &QToolButton::toggled, this, [this](bool on) {
    m_config.set(kKeyZoomActive, on);
  });

  m_alwaysOnTopButton =
    addIconButton(Icon::PinOn, Icon::PinOff, QStringLiteral("desktop_lyric__win_top_on"),
                  QStringLiteral("desktop_lyric__win_top_off"));
  m_alwaysOnTopButton->setCheckable(true);
  connect(m_alwaysOnTopButton, &QToolButton::toggled, this, [this](bool on) {
    m_config.set(kKeyAlwaysOnTop, on);
  });

  // Settings gear: opens the modeless settings dialog. There is no reference
  // i18n key for this button in the language packs, so the tooltip is the
  // literal English string (tr() falls back to the key itself when unknown).
  m_settingsButton = addIconButton(Icon::Settings, QStringLiteral("Lyric settings"));
  connect(m_settingsButton, &QToolButton::clicked, this, &ControlBar::settingsRequested);

  connect(&m_config, &DesktopLyricConfig::settingChanged, this, &ControlBar::onSettingChanged);
  connect(&m_i18n, &TranslationManager::languageChanged, this, &ControlBar::retranslate);

  syncCheckableStates();
  updateVisibility();
}

ControlBar::IconButton* ControlBar::addIconButton(Icon icon, const QString& tooltipKey)
{
  return addIconButton(icon, icon, tooltipKey, tooltipKey);
}

ControlBar::IconButton* ControlBar::addIconButton(Icon iconOn, Icon iconOff,
                                                  const QString& tooltipKeyUnchecked,
                                                  const QString& tooltipKeyChecked)
{
  auto* button = new IconButton(iconOn, iconOff, this);
  m_tooltipBindings.append({button, tooltipKeyUnchecked, tooltipKeyChecked});
  m_layout->addWidget(button);
  return button;
}

void ControlBar::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Full-width rgba(0, 0, 0, .7) strip; only the top corners follow the
  // window's 4px radius (reference .control-bar border-top-left/right-radius:
  // 4px) so the strip meets the content below on a straight edge.
  const qreal r = kBarCornerRadius;
  QPainterPath strip;
  strip.moveTo(0, height());
  strip.lineTo(0, r);
  strip.quadTo(0, 0, r, 0);
  strip.lineTo(width() - r, 0);
  strip.quadTo(width(), 0, width(), r);
  strip.lineTo(width(), height());
  strip.closeSubpath();

  painter.setPen(Qt::NoPen);
  painter.setBrush(kBarFill);
  painter.drawPath(strip);
}

void ControlBar::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  raise(); // Stay above future siblings (the lyric renderer) inside the container.

  // The window's enter/leave events only fire on boundary crossings; a
  // lock->unlock flip while the pointer sits inside emits no new enter, so
  // seed the hover factor from the pointer position (reference #main:hover
  // is a position-based CSS evaluation too).
  setHovered(isPointerInsideWindow());
}

void ControlBar::setRevealMaxOpacity(qreal max)
{
  m_revealMaxOpacity = qBound<qreal>(0.0, max, 1.0);
}

void ControlBar::setHovered(bool hovered)
{
  const double target = hovered ? m_revealMaxOpacity : 0.0;
  if (qAbs(m_hoverFactor - target) < 1e-6) {
    m_hoverAnim->stop();
    return;
  }
  // Retarget smoothly: a running fade is stopped and restarted from the last
  // delivered frame (m_hoverFactor stays live via valueChanged), so a rapid
  // enter/leave never jumps. Durations follow reference animate.less: fadeIn
  // .3s on enter, fadeOut .5s on leave.
  m_hoverAnim->stop();
  m_hoverAnim->setDuration(hovered ? kBarFadeInMs : kBarFadeOutMs);
  m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic); // CSS 'ease' mapping
  m_hoverAnim->setStartValue(m_hoverFactor);
  m_hoverAnim->setEndValue(target);
  m_hoverAnim->start();
}

bool ControlBar::isPointerInsideWindow() const
{
  const QWidget* top = window();
  if (!top || !top->isVisible())
    return false;
  return top->frameGeometry().contains(QCursor::pos());
}

void ControlBar::syncCheckableStates()
{
  const bool zoom = m_config.get(kKeyZoomActive).toBool();
  {
    const QSignalBlocker blocker(m_zoomButton);
    m_zoomButton->setChecked(zoom);
  }

  const bool onTop = m_config.isAlwaysOnTop();
  {
    const QSignalBlocker blocker(m_alwaysOnTopButton);
    m_alwaysOnTopButton->setChecked(onTop);
  }

  retranslate();
}

void ControlBar::retranslate()
{
  for (const TooltipBinding& binding : m_tooltipBindings) {
    binding.button->setToolTip(binding.button->isChecked() ? m_i18n.tr(binding.keyChecked)
                                                           : m_i18n.tr(binding.keyUnchecked));
  }
}

void ControlBar::updateVisibility()
{
  setVisible(!m_config.isLock());
}

void ControlBar::onSettingChanged(const QString& key, const QVariant&)
{
  if (key == kKeyIsLock) {
    updateVisibility();
    return;
  }
  if (key == kKeyZoomActive || key == kKeyAlwaysOnTop)
    syncCheckableStates();
}

void ControlBar::changeFontSize(int delta)
{
  const int current = m_config.fontSize();
  const int clamped = qBound(kFontSizeMin, current + delta, kFontSizeMax);
  if (clamped != current)
    m_config.set(kKeyFontSize, clamped);
}

void ControlBar::changeOpacity(int delta)
{
  const int current = m_config.opacity();
  const int clamped = qBound(kOpacityMin, current + delta, kOpacityMax);
  if (clamped != current)
    m_config.set(kKeyOpacity, clamped);
}

void ControlBar::paintIcon(QPainter& painter, Icon icon, const QRectF& rect) const
{
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.translate(rect.topLeft());
  painter.setPen(QPen(kIconColor, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);

  // All glyphs are drawn in a 16x16 coordinate space.
  switch (icon) {
  case Icon::Close: {
    painter.drawLine(QPointF(4.0, 4.0), QPointF(12.0, 12.0));
    painter.drawLine(QPointF(12.0, 4.0), QPointF(4.0, 12.0));
    break;
  }
  case Icon::Lock: {
    // Shackle: half-circle over the body's top.
    QPainterPath shackle;
    shackle.moveTo(5.8, 7.4);
    shackle.arcTo(QRectF(5.8, 5.4, 4.4, 4.0), 180.0, 180.0);
    painter.drawPath(shackle);
    // Body with a keyhole dot.
    painter.drawRoundedRect(QRectF(5.0, 9.4, 6.0, 4.8), 1.2, 1.2);
    painter.setBrush(kIconColor);
    painter.drawEllipse(QRectF(7.75, 11.1, 0.5, 0.5));
    painter.setBrush(Qt::NoBrush);
    break;
  }
  case Icon::FontIncrease:
  case Icon::FontDecrease: {
    // "A" letterform built from strokes.
    QPainterPath a;
    a.moveTo(3.0, 15.0);
    a.lineTo(8.0, 3.0);
    a.lineTo(13.0, 15.0);
    painter.drawPath(a);
    painter.drawLine(QPointF(5.0, 11.0), QPointF(11.0, 11.0));
    // "+" or "-" in the upper right.
    if (icon == Icon::FontIncrease)
      painter.drawLine(QPointF(14.8, 5.0), QPointF(14.8, 7.0));
    painter.drawLine(QPointF(13.8, 6.0), QPointF(15.8, 6.0));
    break;
  }
  case Icon::OpacityIncrease:
  case Icon::OpacityDecrease: {
    // Lower half of the circle filled (contrast glyph), then the outline.
    QPainterPath lowerHalf;
    lowerHalf.moveTo(12.5, 8.0);
    lowerHalf.arcTo(QRectF(3.5, 3.5, 9.0, 9.0), 0.0, 180.0);
    lowerHalf.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(kIconColor);
    painter.drawPath(lowerHalf);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kIconColor, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawEllipse(QRectF(3.5, 3.5, 9.0, 9.0));
    // "+" or "-" below the circle.
    if (icon == Icon::OpacityIncrease)
      painter.drawLine(QPointF(8.0, 12.5), QPointF(8.0, 15.5));
    painter.drawLine(QPointF(6.0, 14.0), QPointF(10.0, 14.0));
    break;
  }
  case Icon::Zoom:
  case Icon::ZoomOff: {
    // Phone outline with vibration arrows on both sides; the "off" variant
    // adds a slash across the phone.
    painter.drawRoundedRect(QRectF(5.5, 2.5, 5.0, 11.0), 1.5, 1.5);
    painter.drawLine(QPointF(4.2, 5.8), QPointF(2.4, 8.0));
    painter.drawLine(QPointF(4.2, 10.2), QPointF(2.4, 8.0));
    painter.drawLine(QPointF(11.8, 5.8), QPointF(13.6, 8.0));
    painter.drawLine(QPointF(11.8, 10.2), QPointF(13.6, 8.0));
    if (icon == Icon::ZoomOff)
      painter.drawLine(QPointF(3.0, 13.0), QPointF(13.0, 3.0));
    break;
  }
  case Icon::PinOn:
  case Icon::PinOff: {
    // Thumbtack: head circle, shaft, pointed tip; the "off" variant adds a
    // slash through the tack.
    painter.drawEllipse(QRectF(7.0, 2.5, 3.5, 3.5));
    painter.drawLine(QPointF(8.75, 6.0), QPointF(8.75, 11.5));
    QPainterPath tip;
    tip.moveTo(6.75, 11.5);
    tip.lineTo(10.75, 11.5);
    tip.lineTo(8.75, 14.5);
    tip.closeSubpath();
    painter.drawPath(tip);
    if (icon == Icon::PinOff)
      painter.drawLine(QPointF(3.5, 3.5), QPointF(13.5, 13.5));
    break;
  }
  case Icon::Settings: {
    // Simple cog: a center ring with eight short teeth radiating outward.
    constexpr double kPi = std::numbers::pi_v<double>;
    for (int i = 0; i < 8; ++i) {
      const double angle = i * kPi / 4.0;
      const double c = std::cos(angle);
      const double s = std::sin(angle);
      painter.drawLine(QPointF(8.0 + 4.2 * c, 8.0 + 4.2 * s),
                       QPointF(8.0 + 7.0 * c, 8.0 + 7.0 * s));
    }
    painter.drawEllipse(QRectF(3.8, 3.8, 8.4, 8.4));
    break;
  }
  }

  painter.restore();
}
