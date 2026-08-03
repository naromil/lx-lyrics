/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QByteArray>
#include <QColor>
#include <QTimer>
#include <QWidget>

class QPaintEvent;
class QResizeEvent;
class DesktopLyricConfig;

// Spectrum visualizer for the lyric window (port of the reference
// renderer-lyric AudioVisualizer.vue). Draws the 128-byte log-scaled analyser
// frame (protocol §5) as a band of bars across the widget.
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
// The reference schedules each next snapshot with requestAnimationFrame; this
// widget replaces that with a QTimer paced at kFrameIntervalMs (~25 fps): each
// tick renders the current frame and emits analyserDataRequested() so the glue
// asks the host for the next snapshot.
class SpectrumWidget : public QWidget {
    Q_OBJECT

public:
    // The frame is exactly 128 bytes per protocol §5. Default bar color reads
    // from config `desktopLyric.style.lyricPlayedColor` and follows it live
    // unless setBarColor() overrides it explicitly.
    explicit SpectrumWidget(DesktopLyricConfig& config, QWidget* parent = nullptr);

    // Stores one 128-byte spectrum snapshot and repaints. Any other size is
    // dropped loudly (parse at the boundary) — the frame must never enter the
    // render math half-valid.
    void setAnalyserData(const QByteArray& bytes);

    // Gates the render/request loop: true renders frames and requests new
    // snapshots, false idles (loop stopped, nothing painted). The glue feeds
    // this from (isPlay && desktopLyric.audioVisualization).
    void setActive(bool active);

    // Overrides the default config-driven bar color.
    void setBarColor(const QColor& color);

    QSize sizeHint() const override;

signals:
    // Emitted once per frame while active: asks the host for the next
    // analyser snapshot (reference: requestAnimationFrame(getAnalyserDataArray)).
    void analyserDataRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void onFrameTick();
    void onSettingChanged(const QString& key, const QVariant& value);
    // Reference getBarWidth(): the bar slot width for a given widget width.
    static double barWidthFor(int widgetWidth);

    DesktopLyricConfig& m_config;
    QTimer m_frameTimer;
    QColor m_barColor;
    bool m_barColorCustomized = false; // setBarColor() won the default override
    QByteArray m_spectrum;             // trusted 128-byte frame, or empty (none yet)
    bool m_hasFrame = false;
    bool m_active = false;

    // Geometry constants recomputed on resize (reference handleResize()).
    int m_width = 0;
    int m_height = 0;
    double m_maxHeightPerUnit = 0.0; // MAX_HEIGHT: bar height per unit byte value
    double m_barWidth = 0.0;         // getBarWidth(): bar slot width in pixels
};
