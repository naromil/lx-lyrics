/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "engine/lyricclock.h"

#include "engine/checkedarith.h"
#include "engine/lyricplayer.h"

#include <cmath>
#include <limits>

void LyricClock::resync(qint64 rawPositionMs, qint64 totalOffsetMs)
{
  m_rawBasePositionMs = rawPositionMs;
  // Saturated composition: an extreme total offset must clamp the inclusive
  // anchor to the nearest qint64 bound, never wrap into the opposite sign.
  // The raw anchor above is kept VERBATIM — once this composition saturates
  // it is not invertible, so rawPositionMs() must never reconstruct by
  // subtraction.
  m_basePositionMs = saturatingAdd(rawPositionMs, totalOffsetMs);
  if (!m_elapsed.isValid())
    m_elapsed.start();
  else
    m_elapsed.restart();
}

qint64 LyricClock::elapsedMs() const
{
  // Guard the rate-scaled elapsed before qRound64: converting an
  // out-of-range double to qint64 is undefined. Real elapsed values are
  // tiny (milliseconds since resync), so this only fires on absurd inputs.
  // NaN would defeat BOTH comparisons below and reach qRound64 — resolve it
  // to 0 first, so a poisoned rate holds the anchored position instead of
  // invoking undefined behavior.
  const double scaled = m_elapsed.elapsed() * m_rate;
  if (std::isnan(scaled))
    return 0;
  constexpr double kMaxAsDouble = static_cast<double>(std::numeric_limits<qint64>::max());
  constexpr double kMinAsDouble = static_cast<double>(std::numeric_limits<qint64>::min());
  return scaled >= kMaxAsDouble   ? std::numeric_limits<qint64>::max()
         : scaled <= kMinAsDouble ? std::numeric_limits<qint64>::min()
                                  : qRound64(scaled);
}

qint64 LyricClock::currentPositionMs() const
{
  if (!m_elapsed.isValid())
    return m_basePositionMs;
  // Saturating add: the base can already sit at a qint64 bound (an extreme
  // [offset:] tag saturates the anchor), so base + elapsed must not wrap.
  return saturatingAdd(m_basePositionMs, elapsedMs());
}

qint64 LyricClock::rawPositionMs() const
{
  if (!m_elapsed.isValid())
    return m_rawBasePositionMs;
  // Same saturated composition as currentPositionMs() on the RAW anchor:
  // the raw position advances with elapsed playback and the rate, and only
  // clamps at a qint64 bound the caller itself anchored at.
  return saturatingAdd(m_rawBasePositionMs, elapsedMs());
}

void LyricClock::setRate(double rate)
{
  // Deepest defense-in-depth guard: the controller's protocol boundary and
  // the player's setter already validated, but a direct caller can reach the
  // clock. An invalid rate must never replace a valid current one — non-
  // finite values defeat the comparisons in elapsedMs() (NaN slips past both
  // branches into qRound64, undefined behavior), and out-of-range values
  // distort or reverse the position. Rejected BEFORE the assignment, so the
  // last valid rate and the anchored position stay untouched.
  if (!LyricPlayer::isValidPlaybackRate(rate))
    return;
  m_rate = rate;
}

qint64 LyricClock::basePositionMs() const
{
  return m_basePositionMs;
}
