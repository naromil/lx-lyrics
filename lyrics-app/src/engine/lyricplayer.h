/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "engine/lrcparser.h"
#include "engine/lyricclock.h"

// Drives timed LRC line playback: maps a LyricClock position to the current
// line and emits lineChanged as time advances.
//
// Faithful port of LinePlayer from
// references/src/common/utils/lyric-font-player/line-player.js. A single-shot
// timer is scheduled just before the next line's time; refresh() then
// re-schedules against the live clock, so timer drift never accumulates.
class LyricPlayer : public QObject {
  Q_OBJECT
public:
  explicit LyricPlayer(QObject* parent = nullptr);

  // Parses lrc + extendedLyrics and emits lyricsChanged. Returns false when
  // the input is identical to the last call (dedup no-op: nothing re-parsed,
  // no signal) and true once the lyric was re-parsed and signalled.
  bool setLyric(const QString& lrc, const QStringList& extendedLyrics = {});
  void play(qint64 positionMs = 0);
  void pause();
  void stop();
  void setOffset(qint64 offsetMs);
  void setPlaybackRate(double rate);
  void setVertical(bool isVertical);

  qint64 currentPositionMs() const;
  int currentLine() const;
  QString currentText() const;
  bool isPlaying() const;
  const QVector<LrcLine>& lines() const;
  qint64 offset() const;

signals:
  void lineChanged(int line, const QString& text);
  void lyricsChanged();

private:
  void refresh();
  int findCurLineNum(qint64 curTime, int startIndex = 0) const;
  void scheduleNext(qint64 delayMs);

  LyricClock m_clock;
  QTimer m_timer;
  QVector<LrcLine> m_lines;
  // First index in m_lines whose line is NOT static (statics form an
  // un-visitable prefix with timeMs == -1); -1 when there is no timed line.
  int m_firstTimedIndex = 0;
  // Last setLyric inputs, kept so an identical host re-push is a no-op (the
  // Fooyin plugin re-pushes set_info+set_lyric+set_play periodically).
  QString m_lastLrc;
  QStringList m_lastExtendedLyrics;
  qint64 m_userOffsetMs = 0;
  qint64 m_tagOffsetMs = 0;
  double m_rate = 1.0;
  bool m_playing = false;
  int m_curLineNum = 0;
  bool m_lineMode = false;
  qint64 m_totalOffsetMs = 0;

  void onTimerTimeout();
};
