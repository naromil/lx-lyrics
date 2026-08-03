/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "renderer/spectrumwidget.h"

#include <QPainter>
#include <QResizeEvent>
#include <QVariant>

#include "config/desktoplyricconfig.h"

namespace {

constexpr int kFrameBytes = 128;     // protocol §5: exactly 128 bytes per frame
constexpr int kMaxNum = 255;         // reference maxNum: triangle-wave period
constexpr int kBandLimit = 90;       // reference: bins above 90 are skipped
constexpr int kBandSkip = 20;        // reference: band-average starts at bin 20
constexpr int kFrameIntervalMs = 40; // ~25 fps; requestAnimationFrame replacement

const QString kKeyPlayedColor = QStringLiteral("desktopLyric.style.lyricPlayedColor");

} // namespace

SpectrumWidget::SpectrumWidget(DesktopLyricConfig& config, QWidget* parent)
    : QWidget(parent)
    , m_config(config)
    , m_barColor(m_config.playedColor())
{
    setAttribute(Qt::WA_TranslucentBackground); // the window pane shows through
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_frameTimer.setInterval(kFrameIntervalMs);
    connect(&m_frameTimer, &QTimer::timeout, this, &SpectrumWidget::onFrameTick);

    // Default bar color follows the config live; an explicit setBarColor()
    // overrides it (ControlBar-style re-sync, like its checkable buttons).
    connect(&m_config, &DesktopLyricConfig::settingChanged,
            this, &SpectrumWidget::onSettingChanged);
}

QSize SpectrumWidget::sizeHint() const
{
    return QSize(100, 60);
}

void SpectrumWidget::setAnalyserData(const QByteArray& bytes)
{
    if (bytes.size() != kFrameBytes) {
        qWarning() << "SpectrumWidget: dropping analyser frame of" << bytes.size()
                   << "bytes (expected" << kFrameBytes << ")";
        return;
    }
    m_spectrum = bytes;
    m_hasFrame = true;
    update(); // Paint now if active; the paint guard idles it otherwise.
}

void SpectrumWidget::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;

    if (active) {
        m_frameTimer.start();
        onFrameTick(); // First request immediately; the timer keeps it flowing.
    } else {
        m_frameTimer.stop();
        m_spectrum.clear();
        m_hasFrame = false;
    }
    update();
}

void SpectrumWidget::setBarColor(const QColor& color)
{
    m_barColor = color;
    m_barColorCustomized = true;
    update();
}

void SpectrumWidget::onFrameTick()
{
    if (!m_active)
        return;
    update();                     // Render the current frame...
    emit analyserDataRequested(); // ...then schedule the next snapshot.
}

void SpectrumWidget::onSettingChanged(const QString& key, const QVariant&)
{
    if (key == kKeyPlayedColor && !m_barColorCustomized)
        setBarColor(m_config.playedColor());
}

void SpectrumWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    // Idle (paused/stopped/setting off): paint nothing; the pane stays clean.
    if (!m_active || !m_hasFrame)
        return;

    // Reference renderFrame(): band-average first, then draw the bars.
    double frequencyAvg = 0.0;
    for (int i = 0; i < m_spectrum.size(); ++i) {
        // Reference num mapping is a triangle wave over maxNum. Every input
        // index is < 255, so num == i here; kept general to match the .vue.
        const int mult = i / kMaxNum;
        const int num = mult % 2 == 0
            ? (i - kMaxNum * mult)
            : (kMaxNum - (i - kMaxNum * mult));
        const int spectrum = num > kBandLimit ? 0 : quint8(m_spectrum.at(num + kBandSkip));
        frequencyAvg += spectrum * 1.4;
    }
    frequencyAvg /= m_spectrum.size(); // dataArray.length
    frequencyAvg *= 1.6;
    frequencyAvg /= kMaxNum;

    QPainter painter(this);
    painter.setPen(Qt::NoPen);

    double x = 0.0;
    for (int i = 0; i < m_spectrum.size(); ++i) {
        if (x > m_width)
            break; // Reference: stop once the bars run past the canvas.
        const int byte = quint8(m_spectrum.at(i));
        const double barHeight = (byte * frequencyAvg + byte * 0.42) * m_maxHeightPerUnit;
        painter.fillRect(QRectF(x, m_height - barHeight, m_barWidth, barHeight), m_barColor);
        x += m_barWidth;
    }
}

void SpectrumWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    // Reference handleResize(): recompute MAX_HEIGHT and the bar slot width.
    m_width = width();
    m_height = height();
    m_maxHeightPerUnit = qRound(m_height * 0.46 / kMaxNum * 10000.0) / 10000.0;
    m_barWidth = barWidthFor(m_width);
    update();
}

double SpectrumWidget::barWidthFor(int widgetWidth)
{
    // Reference getBarWidth(): 2.5x slots at normal widths; ultra-wide widgets
    // fall back to tight slots so all 128 bars fit.
    const double wideBar = (widgetWidth / 128.0) * 2.5;
    const double slot = widgetWidth / 86.0;
    const double diff = wideBar - slot;
    if (diff > 32.0)
        return widgetWidth / 128.0;
    if (diff > 12.0)
        return slot;
    return wideBar;
}
