/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QtGlobal>

#include <limits>

// Saturating qint64 arithmetic for the lyric timing path: timestamp/offset
// composition and recovery must never invoke signed overflow (UB). Both
// helpers clamp to the nearest representable qint64 value instead of
// wrapping, so an extreme [offset:] tag (the parser clamps it to qint64
// bounds), a boundary user offset, or a pathological line timestamp degrade
// to a finite boundary value — never to an opposite-sign position that would
// jump the lyric or spin the timer.

// a + b, clamped to [qint64::min(), qint64::max()].
inline qint64 saturatingAdd(qint64 a, qint64 b) noexcept
{
  if (b > 0 && a > std::numeric_limits<qint64>::max() - b)
    return std::numeric_limits<qint64>::max();
  if (b < 0 && a < std::numeric_limits<qint64>::min() - b)
    return std::numeric_limits<qint64>::min();
  return a + b;
}

// a - b, clamped to [qint64::min(), qint64::max()]. Written independently
// (NOT as saturatingAdd(a, -b)): negating b == qint64::min() would itself
// overflow.
inline qint64 saturatingSub(qint64 a, qint64 b) noexcept
{
  if (b < 0 && a > std::numeric_limits<qint64>::max() + b)
    return std::numeric_limits<qint64>::max();
  if (b > 0 && a < std::numeric_limits<qint64>::min() + b)
    return std::numeric_limits<qint64>::min();
  return a - b;
}
