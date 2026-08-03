/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "renderer/controlbar.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>

#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"

namespace {

constexpr int kBarHeight = 28;
constexpr int kButtonSize = 22;
constexpr int kIconSize = 16;
constexpr int kPillInset = 4;         // horizontal float margin inside the window frame
constexpr int kPillCornerRadius = 8;
constexpr int kPillButtonPadding = 2; // buttons inset from the pill edge

constexpr int kFontStep = 2;
constexpr int kFontSizeMin = 10;
constexpr int kFontSizeMax = 80;
constexpr int kOpacityStep = 2;
constexpr int kOpacityMin = 6;
constexpr int kOpacityMax = 100;

const QColor kPillFill(0, 0, 0, 115);        // rgba(0, 0, 0, 0.45)
const QColor kPillBorder(255, 255, 255, 45); // thin light border, like the window pane
const QColor kIconColor(255, 255, 255, 235); // white/light-gray strokes
const QColor kHoverFill(255, 255, 255, 26);  // rgba(255, 255, 255, 0.1)
const QColor kCheckedFill(255, 255, 255, 51); // rgba(255, 255, 255, 0.2)

const QString kKeyIsLock = QStringLiteral("desktopLyric.isLock");
const QString kKeyAlwaysOnTop = QStringLiteral("desktopLyric.isAlwaysOnTop");
const QString kKeyZoomActive = QStringLiteral("desktopLyric.style.isZoomActiveLrc");
const QString kKeyFontSize = QStringLiteral("desktopLyric.style.fontSize");
const QString kKeyOpacity = QStringLiteral("desktopLyric.style.opacity");

} // namespace

// Icon button with a fully custom paintEvent: draws a subtle hover/checked
// backdrop and then the requested glyph with QPainter. Checkable buttons pick
// between two glyphs so the icon mirrors the reference ControlBar.vue
// (e.g. the "zoom off" glyph shows while zoom is active).
class ControlBar::IconButton : public QToolButton {
public:
    IconButton(Icon iconOn, Icon iconOff, ControlBar* bar)
        : QToolButton(bar)
        , m_iconOn(iconOn)
        , m_iconOff(iconOff)
        , m_bar(bar)
    {
        setFixedSize(kButtonSize, kButtonSize);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoRaise(true);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF backdrop = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
        if (isChecked()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(kCheckedFill);
            painter.drawEllipse(backdrop);
        } else if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(kHoverFill);
            painter.drawEllipse(backdrop);
        }

        const Icon icon = isChecked() ? m_iconOn : m_iconOff;
        const QRectF iconRect = QRectF((width() - kIconSize) / 2.0,
                                       (height() - kIconSize) / 2.0,
                                       kIconSize, kIconSize);
        m_bar->paintIcon(painter, icon, iconRect);
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
    setAttribute(Qt::WA_TranslucentBackground); // the pill shape shows through

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(kPillInset + kPillButtonPadding, 0,
                                 kPillInset + kPillButtonPadding, 0);
    m_layout->setSpacing(2);

    // Reference order (ControlBar.vue): close, lock, font+, font-, opacity+,
    // opacity-, zoom, always-on-top.
    auto* closeButton = addIconButton(Icon::Close, QStringLiteral("desktop_lyric__close"));
    connect(closeButton, &QToolButton::clicked, this, [] { QApplication::quit(); });

    auto* lockButton = addIconButton(Icon::Lock, QStringLiteral("desktop_lyric__lock"));
    connect(lockButton, &QToolButton::clicked, this, [this] { m_config.set(kKeyIsLock, true); });

    auto* fontIncreaseButton = addIconButton(Icon::FontIncrease, QStringLiteral("desktop_lyric__font_increase"));
    connect(fontIncreaseButton, &QToolButton::clicked, this, [this] { changeFontSize(+kFontStep); });

    auto* fontDecreaseButton = addIconButton(Icon::FontDecrease, QStringLiteral("desktop_lyric__font_decrease"));
    connect(fontDecreaseButton, &QToolButton::clicked, this, [this] { changeFontSize(-kFontStep); });

    auto* opacityIncreaseButton = addIconButton(Icon::OpacityIncrease, QStringLiteral("desktop_lyric__opacity_increase"));
    connect(opacityIncreaseButton, &QToolButton::clicked, this, [this] { changeOpacity(+kOpacityStep); });

    auto* opacityDecreaseButton = addIconButton(Icon::OpacityDecrease, QStringLiteral("desktop_lyric__opacity_decrease"));
    connect(opacityDecreaseButton, &QToolButton::clicked, this, [this] { changeOpacity(-kOpacityStep); });

    m_zoomButton = addIconButton(Icon::ZoomOff, Icon::Zoom,
                                 QStringLiteral("desktop_lyric__lrc_active_zoom_on"),
                                 QStringLiteral("desktop_lyric__lrc_active_zoom_off"));
    m_zoomButton->setCheckable(true);
    connect(m_zoomButton, &QToolButton::toggled, this, [this](bool on) {
        m_config.set(kKeyZoomActive, on);
    });

    m_alwaysOnTopButton = addIconButton(Icon::PinOn, Icon::PinOff,
                                        QStringLiteral("desktop_lyric__win_top_on"),
                                        QStringLiteral("desktop_lyric__win_top_off"));
    m_alwaysOnTopButton->setCheckable(true);
    connect(m_alwaysOnTopButton, &QToolButton::toggled, this, [this](bool on) {
        m_config.set(kKeyAlwaysOnTop, on);
    });

    connect(&m_config, &DesktopLyricConfig::settingChanged,
            this, &ControlBar::onSettingChanged);
    connect(&m_i18n, &TranslationManager::languageChanged,
            this, &ControlBar::retranslate);

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
    m_tooltipBindings.append({ button, tooltipKeyUnchecked, tooltipKeyChecked });
    m_layout->addWidget(button);
    return button;
}

void ControlBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF pill = QRectF(rect()).adjusted(kPillInset + 0.5, 0.5,
                                                -(kPillInset + 0.5), -0.5);
    painter.setPen(QPen(kPillBorder, 1.0));
    painter.setBrush(kPillFill);
    painter.drawRoundedRect(pill, kPillCornerRadius, kPillCornerRadius);
}

void ControlBar::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    raise(); // Stay above future siblings (the lyric renderer) inside the container.
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
        binding.button->setToolTip(binding.button->isChecked()
                                       ? m_i18n.tr(binding.keyChecked)
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
    }

    painter.restore();
}
