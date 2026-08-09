/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "engine/lyricclock.h"

void LyricClock::resync(qint64 positionMs)
{
  m_basePositionMs = positionMs;
  if (!m_elapsed.isValid())
    m_elapsed.start();
  else
    m_elapsed.restart();
}

qint64 LyricClock::currentPositionMs() const
{
  if (!m_elapsed.isValid())
    return m_basePositionMs;
  return m_basePositionMs + qRound64(m_elapsed.elapsed() * m_rate);
}

void LyricClock::setRate(double rate)
{
  m_rate = rate;
}

qint64 LyricClock::basePositionMs() const
{
  return m_basePositionMs;
}
