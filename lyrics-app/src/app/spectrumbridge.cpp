/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "app/spectrumbridge.h"

#include "bridge/wsclient.h"
#include "config/desktoplyricconfig.h"
#include "renderer/spectrumwidget.h"

namespace {

const QString kKeyVisualization = QStringLiteral("desktopLyric.audioVisualization");

} // namespace

SpectrumBridge::SpectrumBridge(SpectrumWidget* spectrum, WsClient* ws,
                               DesktopLyricConfig& config, QObject* parent)
    : QObject(parent)
    , m_spectrum(spectrum)
    , m_ws(ws)
    , m_config(config)
    , m_visualizationEnabled(m_config.get(kKeyVisualization).toBool())
{
    connect(m_ws, &WsClient::analyserDataReceived,
            m_spectrum, &SpectrumWidget::setAnalyserData);
    connect(m_spectrum, &SpectrumWidget::analyserDataRequested,
            m_ws, &WsClient::sendGetAnalyserData);

    // The play gate, mirroring PauseHide: set_info/set_status carry isPlay;
    // set_play implies true; set_pause/set_stop imply false.
    connect(m_ws, &WsClient::infoReceived, this,
            [this](const TrackSnapshot& info) { setPlaying(info.isPlay); });
    connect(m_ws, &WsClient::statusReceived, this,
            [this](const PlaybackSnapshot& status) { setPlaying(status.isPlay); });
    connect(m_ws, &WsClient::playReceived, this, [this](qint64) { setPlaying(true); });
    connect(m_ws, &WsClient::pauseReceived, this, [this] { setPlaying(false); });
    connect(m_ws, &WsClient::stopReceived, this, [this] { setPlaying(false); });

    connect(&m_config, &DesktopLyricConfig::settingChanged,
            this, &SpectrumBridge::onSettingChanged);

    updateActive();
}

void SpectrumBridge::setPlaying(bool playing)
{
    m_playing = playing;
    updateActive();
}

void SpectrumBridge::updateActive()
{
    m_spectrum->setActive(m_playing && m_visualizationEnabled);
}

void SpectrumBridge::onSettingChanged(const QString& key, const QVariant& value)
{
    if (key == kKeyVisualization) {
        m_visualizationEnabled = value.toBool();
        updateActive();
    }
}
