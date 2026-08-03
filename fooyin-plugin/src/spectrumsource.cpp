/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "spectrumsource.h"

#include "hostserver.h"

#include <core/engine/enginecontroller.h>
#include <core/engine/visualisationservice.h>

#include <algorithm>
#include <cmath>

namespace {
// protocol.md §5: the analyser frame is exactly 128 bytes. The pull session
// runs an FFT of 256 samples -> (256/2)+1 = 129 magnitude bins (DC..Nyquist);
// scaleToBytes keeps the first 128 and drops the Nyquist bin, which carries
// negligible audible energy. SCALE is pinned by the protocol: a full-scale
// magnitude of 1.0 maps to exactly 255.
constexpr int kProtocolFrameSize = 128;
constexpr int kSpectrumFftSize   = 256;
constexpr double kScale          = 255.0;
const double kLogOf256           = std::log10(256.0);

/// protocol.md §5: byte = clamp(round(255 * log10(1 + m*SCALE) / log10(256)), 0, 255).
unsigned char logScaleMagnitude(float magnitude)
{
    if (magnitude <= 0.0f) {
        return 0;
    }
    const double scaled = 255.0 * std::log10(1.0 + static_cast<double>(magnitude) * kScale) / kLogOf256;
    return static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(scaled)), 0, 255));
}
} // namespace

SpectrumSource::SpectrumSource(Fooyin::EngineController* engine, HostServer* host, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_host(host)
{ }

QByteArray SpectrumSource::scaleToBytes(const QVector<float>& bins)
{
    QByteArray frame(kProtocolFrameSize, 0);
    const int used = std::min(static_cast<int>(bins.size()), kProtocolFrameSize);
    for (int i = 0; i < used; ++i) {
        frame[i] = static_cast<char>(logScaleMagnitude(bins.at(i)));
    }
    return frame;
}

void SpectrumSource::onAnalyserDataRequested()
{
    if (m_host == nullptr) {
        return;
    }

    QVector<float> bins;
    const std::shared_ptr<Fooyin::VisualisationSession> session = ensureSession();
    if (session != nullptr) {
        Fooyin::VisualisationSession::SpectrumWindow window;
        const uint64_t now = session->currentTimeMs();
        if (session->getSpectrumWindowEndingAt(window, now, kSpectrumFftSize) && window.isValid()) {
            const std::span<const float> windowBins = window.bins();
            bins = QVector<float>(windowBins.begin(), windowBins.end());
        }
    }

    // No data (engine not playing / session not yet warm) -> a deterministic
    // zero frame: the app's widget stays alive and the protocol never sees a
    // wrong-size payload.
    m_host->sendAnalyserData(scaleToBytes(bins));
}

std::shared_ptr<Fooyin::VisualisationSession> SpectrumSource::ensureSession()
{
    if (m_session != nullptr) {
        return m_session;
    }
    if (m_engine == nullptr) {
        return nullptr;
    }
    Fooyin::VisualisationService* service = m_engine->visualisationService();
    if (service == nullptr) {
        return nullptr;
    }
    m_session = service->createSession();
    return m_session;
}
