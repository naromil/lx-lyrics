/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QObject>
#include <QVariant>

class DesktopLyricConfig;
class SpectrumTransport;
class SpectrumWidget;

// Spectrum-only wiring for the lyric window's visualizer (task 2.11).
//
// Owns the three couplings the visualizer needs:
//   - transport frameReceived            -> SpectrumWidget::setAnalyserData
//   - SpectrumWidget::analyserDataRequested -> transport requestFrame
//   - the active gate: the render loop runs only while the transport reports
//     playing AND `desktopLyric.audioVisualization` is on.
// The transport says WHERE frames come from (the player feed, or the
// synthetic `--demo` feed), so both modes share this one gate and loop.
class SpectrumBridge : public QObject {
  Q_OBJECT

public:
  explicit SpectrumBridge(SpectrumWidget* spectrum, SpectrumTransport* transport,
                          DesktopLyricConfig& config, QObject* parent = nullptr);

  // Feeds the current play boolean, mirroring PauseHide::setPlayState. The
  // transport's playStateChanged drives it; callers may also push it directly.
  void setPlaying(bool playing);

private:
  void updateActive();
  void onSettingChanged(const QString& key, const QVariant& value);

  SpectrumWidget* m_spectrum;
  SpectrumTransport* m_transport;
  DesktopLyricConfig& m_config;
  bool m_playing = false;
  bool m_visualizationEnabled = false;
};
