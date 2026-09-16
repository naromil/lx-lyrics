/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "renderer/spectrumwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QtGlobal>

#include <cstring>

namespace {

constexpr int kFrameBytes = 128;     // protocol §5: exactly 128 bytes per frame
constexpr int kMaxNum = 255;         // reference maxNum: triangle-wave period
constexpr int kBandLimit = 90;       // reference: bins above 90 are skipped
constexpr int kBandSkip = 20;        // reference: band-average starts at bin 20
constexpr int kFrameIntervalMs = 40; // 25 fps; the reference's rAF replacement
// Reference canvas colour: `let themeColor = 'rgba(255, 255, 255, .12)'`
// (AudioVisualizer.vue:66). Always opaque-white-tinted: the visualizer is a
// backdrop, not a themed element.
constexpr int kBarAlpha = 31; // round(0.12 * 255)
// Reference `#main { overflow: hidden; border-radius: 4px }` clips the canvas
// to the pane's rounded corners; the pane itself is painted by LyricWindow
// (kRadiusBorder there), so the backdrop clips where the pane does.
constexpr int kCornerRadius = 4;

} // namespace

SpectrumWidget::SpectrumWidget(QWidget* parent)
  : QWidget(parent)
{
  setAttribute(Qt::WA_TranslucentBackground);     // the window pane shows through
  setAttribute(Qt::WA_TransparentForMouseEvents); // reference pointer-events: none
  m_spectrum.resize(kFrameBytes);                 // fixed-size buffer: the hot path never allocates

  m_frameTimer.setInterval(kFrameIntervalMs);
  connect(&m_frameTimer, &QTimer::timeout, this, &SpectrumWidget::onFrameTick);
}

void SpectrumWidget::setAnalyserData(const QByteArray& bytes)
{
  if (bytes.size() != kFrameBytes) {
    qWarning() << "SpectrumWidget: dropping analyser frame of" << bytes.size() << "bytes (expected"
               << kFrameBytes << ")";
    return;
  }
  std::memcpy(m_spectrum.data(), bytes.constData(), kFrameBytes);
  m_hasFrame = true;
  if (m_active)
    update(); // Paint now; idle frames are stored but not drawn.
}

void SpectrumWidget::setActive(bool active)
{
  if (m_active == active)
    return;
  m_active = active;
  if (!active)
    m_hasFrame = false; // Idle renders nothing (see the pause note in the header).
  updateRunning();
  update();
}

void SpectrumWidget::setBodyOpacity(qreal opacity)
{
  m_bodyOpacity = qBound(0.0, opacity, 1.0);
  update();
}

void SpectrumWidget::onFrameTick()
{
  if (!m_active)
    return;
  update();                     // Render the current frame...
  emit analyserDataRequested(); // ...then schedule the next snapshot.
}

void SpectrumWidget::updateRunning()
{
  const bool run = m_active && isVisible();
  if (run == m_frameTimer.isActive())
    return;

  if (run) {
    m_frameTimer.start();
    onFrameTick(); // First request immediately; the timer keeps it flowing.
  } else {
    m_frameTimer.stop();
  }
}

void SpectrumWidget::paintEvent(QPaintEvent* event)
{
  QWidget::paintEvent(event);

  // Idle (paused/stopped/setting off): paint nothing; the pane stays clean.
  if (!m_active || !m_hasFrame)
    return;

  // Reference renderFrame(): band-average first, then draw the bars.
  double frequencyAvg = 0.0;
  for (int i = 0; i < kFrameBytes; ++i) {
    // Reference num mapping is a triangle wave over maxNum. Every input
    // index is < 255, so num == i here; kept general to match the .vue.
    const int mult = i / kMaxNum;
    const int num = mult % 2 == 0 ? (i - kMaxNum * mult) : (kMaxNum - (i - kMaxNum * mult));
    const int spectrum = num > kBandLimit ? 0 : quint8(m_spectrum.at(num + kBandSkip));
    frequencyAvg += spectrum * 1.4;
  }
  frequencyAvg /= kFrameBytes; // dataArray.length
  frequencyAvg *= 1.6;
  frequencyAvg /= kMaxNum;

  QPainter painter(this);
  painter.setPen(Qt::NoPen);
  // The reference compounds the canvas colour with the window body dimming
  // (`body { opacity: .8 }`).
  painter.setOpacity(m_bodyOpacity);
  QPainterPath clip;
  clip.addRoundedRect(QRectF(rect()), kCornerRadius, kCornerRadius);
  painter.setClipPath(clip);

  const QColor barColor(255, 255, 255, kBarAlpha);
  double x = 0.0;
  for (int i = 0; i < kFrameBytes; ++i) {
    if (x > m_width)
      break; // Reference: stop once the bars run past the canvas.
    const int byte = quint8(m_spectrum.at(i));
    const double barHeight = (byte * frequencyAvg + byte * 0.42) * m_maxHeightPerUnit;
    painter.fillRect(QRectF(x, m_height - barHeight, m_barWidth, barHeight), barColor);
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

void SpectrumWidget::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  updateRunning();
}

void SpectrumWidget::hideEvent(QHideEvent* event)
{
  QWidget::hideEvent(event);
  updateRunning();
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
