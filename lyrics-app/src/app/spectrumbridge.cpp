/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "app/spectrumbridge.h"

#include "app/spectrumtransport.h"
#include "config/desktoplyricconfig.h"
#include "renderer/spectrumwidget.h"

namespace {

const QString kKeyVisualization = QStringLiteral("desktopLyric.audioVisualization");

} // namespace

SpectrumBridge::SpectrumBridge(SpectrumWidget* spectrum, SpectrumTransport* transport,
                               DesktopLyricConfig& config, QObject* parent)
  : QObject(parent)
  , m_spectrum(spectrum)
  , m_transport(transport)
  , m_config(config)
  , m_playing(m_transport->isPlaying())
  , m_visualizationEnabled(m_config.get(kKeyVisualization).toBool())
{
  connect(m_transport, &SpectrumTransport::frameReceived, m_spectrum,
          &SpectrumWidget::setAnalyserData);
  connect(m_spectrum, &SpectrumWidget::analyserDataRequested, m_transport,
          &SpectrumTransport::requestFrame);
  connect(m_transport, &SpectrumTransport::playStateChanged, this, &SpectrumBridge::setPlaying);
  connect(&m_config, &DesktopLyricConfig::settingChanged, this, &SpectrumBridge::onSettingChanged);

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
