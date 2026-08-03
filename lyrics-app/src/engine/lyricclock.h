/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QElapsedTimer>
#include <QtGlobal>

// Steady monotonic clock that maps elapsed wall time to a lyric position.
//
// Faithful port of the JS lyric player's
// _currentTime() = (getNow() - _performanceTime) * _rate + _startTime.
// A fresh clock is unanchored: currentPositionMs() reports the base position
// until resync() anchors it to the QElapsedTimer.
class LyricClock {
public:
    // Re-anchor the clock: the given position is the position "now".
    // The first call starts the timer; later calls restart it.
    void resync(qint64 positionMs);

    // Current position: base + elapsed time scaled by the playback rate.
    qint64 currentPositionMs() const;

    // Change the playback rate. Does NOT re-anchor here: the caller calls
    // resync() when a rate change must realign the position.
    void setRate(double rate);

    qint64 basePositionMs() const;

private:
    qint64 m_basePositionMs = 0;
    QElapsedTimer m_elapsed;
    double m_rate = 1.0;
};
