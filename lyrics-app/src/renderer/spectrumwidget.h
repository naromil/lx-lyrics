/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QByteArray>
#include <QTimer>
#include <QWidget>

class QPaintEvent;
class QResizeEvent;

// Spectrum visualizer for the lyric window (port of the reference
// renderer-lyric AudioVisualizer.vue).
//
// PLACEMENT — the reference mounts a full-window <canvas> as the #main
// backdrop (`.content { position: absolute; inset: 0; pointer-events: none;
// z-index: -1 }`, AudioVisualizer.vue:181-189) so the bars sit BEHIND the
// lyric lines across the whole window. This widget is that backdrop:
// LyricWindow gives it the content container's whole rect and keeps it at the
// bottom of the child stack (below the lyric renderer and the control bar) and
// mouse-transparent.
//
// DIMMING — the reference colour is the hardcoded rgba(255,255,255,.12)
// compounded by the lyric window's `body { opacity: .8 }`. The widget paints
// that dim itself (painter opacity, setBodyOpacity) rather than carrying a
// QGraphicsOpacityEffect: a second effect nested inside the content
// container's fade effect makes Qt drop every paint the widget makes, so the
// window showed an empty backdrop while frames were arriving. ControlBar
// avoids the same conflict the same way (see lyricwindow.cpp).
//
// The VISUAL LOGIC is a faithful port of the .vue renderFrame():
//   1. A band-average frequencyAvg is computed from source bins `num + 20`
//      (num 0..90, the triangle-wave num mapping from the reference), scaled
//      with the renderer-lyric constants (x1.4, then /128, then x1.6, /255).
//   2. Each bar i draws with height
//      byte[i] * (frequencyAvg + 0.42) * MAX_HEIGHT, where
//      MAX_HEIGHT = round(height * 0.46 / 255 * 10000) / 10000, and bars are
//      spaced with the reference getBarWidth() (2.5x slots at normal widths,
//      tighter on very wide widgets so more of the 128 bars fit).
//
// CADENCE — the reference schedules each next snapshot with
// requestAnimationFrame (~60 fps). This widget deliberately runs a QTimer at
// kFrameIntervalMs (40 ms = 25 fps): 2.4x cheaper for an effect that is
// already smoothed by the host's log-scaled frame, and it keeps the pull loop
// off the render path. Each tick renders the current frame and asks the
// transport for the next snapshot. The loop additionally stops whenever the
// widget is not visible: a hidden lyric window must not keep pulling frames
// (and making the host compute FFTs).
class SpectrumWidget : public QWidget {
  Q_OBJECT

public:
  explicit SpectrumWidget(QWidget* parent = nullptr);

  // Stores one 128-byte spectrum snapshot and repaints. Any other size is
  // dropped loudly (parse at the boundary) — the frame must never enter the
  // render math half-valid.
  void setAnalyserData(const QByteArray& bytes);

  // Gates the render/request loop: true renders frames and asks for new
  // snapshots, false idles (loop stopped, nothing painted). The glue feeds
  // this from (isPlay && desktopLyric.audioVisualization).
  void setActive(bool active);

  // The lyric window's body dimming (LyricWindow::kBodyOpacity), compounded
  // with the reference canvas colour — the value ControlBar receives through
  // setRevealMaxOpacity. Default 1.0 (undimmed).
  void setBodyOpacity(qreal opacity);

signals:
  // Emitted once per frame while running: asks the transport for the next
  // analyser snapshot (reference: requestAnimationFrame(getAnalyserDataArray)).
  void analyserDataRequested();

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  void onFrameTick();
  // Starts/stops the frame timer to match (active && visible).
  void updateRunning();
  // Reference getBarWidth(): the bar slot width for a given widget width.
  static double barWidthFor(int widgetWidth);

  QTimer m_frameTimer;
  QByteArray m_spectrum; // 128-byte frame buffer, allocated once (no per-frame alloc)
  bool m_hasFrame = false;
  bool m_active = false;
  qreal m_bodyOpacity = 1.0;

  // Geometry constants recomputed on resize (reference handleResize()).
  int m_width = 0;
  int m_height = 0;
  double m_maxHeightPerUnit = 0.0; // MAX_HEIGHT: bar height per unit byte value
  double m_barWidth = 0.0;         // getBarWidth(): bar slot width in pixels
};
