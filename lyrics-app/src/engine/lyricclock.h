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
//
// resync() anchors TWO positions at once: the caller's RAW position (no
// offset applied) and the offset-INCLUSIVE position the lyric timing runs
// on. The inclusive composition is NOT invertible once it saturates (raw +
// offset at a qint64 bound loses the raw part), so the raw position is kept
// as its own anchor and recovered via rawPositionMs() — never reconstructed
// by subtracting the offset from the inclusive position.
class LyricClock {
public:
  // Re-anchor the clock: rawPositionMs is the caller's position "now" and
  // totalOffsetMs is applied to it for the offset-inclusive position.
  // The first call starts the timer; later calls restart it.
  void resync(qint64 rawPositionMs, qint64 totalOffsetMs);

  // Offset-inclusive position: inclusive anchor + elapsed time scaled by
  // the playback rate (saturated composition). Drives line selection.
  qint64 currentPositionMs() const;

  // Raw position: raw anchor + the SAME elapsed time scaled by the playback
  // rate. Independently recoverable while playing — including when the
  // inclusive position saturates — so a live re-anchor (offset/rate/lyric
  // change) can replay the caller's position instead of the bogus value a
  // subtractive reconstruction would yield there.
  qint64 rawPositionMs() const;

  // Change the playback rate. Does NOT re-anchor here: the caller calls
  // resync() when a rate change must realign the position.
  //
  // Defense in depth: a direct caller can bypass the player's and the
  // controller's validation, so the clock itself rejects any rate outside
  // the supported playback-rate range (LyricPlayer's policy) — including
  // NaN, infinities, zero, negatives and tiny values — and keeps the last
  // valid rate. Rejected, never clamped: an invalid rate must not silently
  // distort or reverse the timing.
  void setRate(double rate);

  // Test accessor: the active playback rate (1.0 until a valid rate is set;
  // rejected setRate calls leave it unchanged).
  double rate() const { return m_rate; }

  qint64 basePositionMs() const;

private:
  // Rate-scaled elapsed wall time since the last resync, resolved to a safe
  // qint64: NaN holds the anchored position (elapsed 0), infinities saturate
  // to the qint64 bounds, anything else rounds (the guard before qRound64
  // keeps the narrowing defined).
  qint64 elapsedMs() const;

  qint64 m_rawBasePositionMs = 0;
  qint64 m_basePositionMs = 0;
  QElapsedTimer m_elapsed;
  double m_rate = 1.0;
};
