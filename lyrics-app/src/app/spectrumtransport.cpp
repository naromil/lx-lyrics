/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "app/spectrumtransport.h"

#include "host/feedreader.h"

#include <QtGlobal>

#include <cmath>

namespace {

constexpr int kFrameBytes = 128;    // protocol §5
constexpr double kPhaseStep = 0.05; // 25 fps -> ~1.25 cycles/second

} // namespace

FeedSpectrumTransport::FeedSpectrumTransport(FeedReader* feed, QObject* parent)
  : SpectrumTransport(parent)
  , m_feed(feed)
{
  connect(m_feed, &FeedReader::analyserDataReceived, this, &SpectrumTransport::frameReceived);
  connect(m_feed, &FeedReader::infoReceived, this, [this](const TrackSnapshot& info) {
    if (m_playing != info.isPlay) {
      m_playing = info.isPlay;
      emit playStateChanged(m_playing);
    }
  });
  connect(m_feed, &FeedReader::statusReceived, this, [this](const PlaybackSnapshot& status) {
    if (m_playing != status.isPlay) {
      m_playing = status.isPlay;
      emit playStateChanged(m_playing);
    }
  });
  connect(m_feed, &FeedReader::playReceived, this, [this](qint64) {
    if (!m_playing) {
      m_playing = true;
      emit playStateChanged(true);
    }
  });
  connect(m_feed, &FeedReader::pauseReceived, this, [this] {
    if (m_playing) {
      m_playing = false;
      emit playStateChanged(false);
    }
  });
  connect(m_feed, &FeedReader::stopReceived, this, [this] {
    if (m_playing) {
      m_playing = false;
      emit playStateChanged(false);
    }
  });
}

void FeedSpectrumTransport::requestFrame()
{
  // Suppressed by the reader until the host's hello declared an analyser
  // (protocol §5), so a host without one never sees the request.
  m_feed->requestAnalyserData();
}

DemoSpectrumTransport::DemoSpectrumTransport(QObject* parent)
  : SpectrumTransport(parent)
  , m_frame(kFrameBytes, '\0')
{
}

void DemoSpectrumTransport::requestFrame()
{
  const double t = static_cast<double>(m_tick++) * kPhaseStep;
  const double beat = 0.62 + 0.38 * std::sin(t * 2.0); // 0.24..1.0, ~2.5 s period
  for (int i = 0; i < kFrameBytes; ++i) {
    const double tilt = 0.55 + 0.45 * std::exp(-i / 40.0);   // bass-heavy
    const double swell = 0.5 + 0.5 * std::sin(t + i * 0.11); // moving ripple
    const double value = 255.0 * tilt * (0.35 + 0.65 * swell) * beat;
    m_frame[i] = static_cast<char>(qBound(0, static_cast<int>(std::lround(value)), 255));
  }
  emit frameReceived(m_frame);
}
