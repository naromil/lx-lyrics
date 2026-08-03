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
class SpectrumWidget;
class WsClient;

// Spectrum-only wiring for the lyric window's visualizer (task 2.11). The full
// music-state machine arrives in task 2.13; this stays deliberately tiny.
//
// Owns the three couplings the visualizer needs:
//   - WsClient::analyserDataReceived  -> SpectrumWidget::setAnalyserData
//   - SpectrumWidget::analyserDataRequested -> WsClient::sendGetAnalyserData
//   - the active gate: the render loop runs only while the host reports
//     playing (isPlay via set_info/set_status, implied true by set_play, false
//     by set_pause/set_stop) AND `desktopLyric.audioVisualization` is on.
class SpectrumBridge : public QObject {
    Q_OBJECT

public:
    explicit SpectrumBridge(SpectrumWidget* spectrum, WsClient* ws,
                            DesktopLyricConfig& config, QObject* parent = nullptr);

    // Feeds the current play boolean, mirroring PauseHide::setPlayState: any
    // state message that conveys the play boolean calls this.
    void setPlaying(bool playing);

private:
    void updateActive();
    void onSettingChanged(const QString& key, const QVariant& value);

    SpectrumWidget* m_spectrum;
    WsClient* m_ws;
    DesktopLyricConfig& m_config;
    bool m_playing = false;
    bool m_visualizationEnabled = false;
};
