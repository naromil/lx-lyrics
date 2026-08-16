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

#include <optional>

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
  // Supported playback-rate range (defense in depth, task F): rates outside
  // [kMinPlaybackRate, kMaxPlaybackRate] — including zero, negatives, NaN
  // and infinities — are REJECTED by setPlaybackRate, never clamped; the
  // current rate stays. The range is conservative for a lyrics overlay
  // (0.25x–4.0x) and keeps the delay math (division, qint64 conversion) away
  // from overflow and 0 ms timer spins.
  static constexpr double kMinPlaybackRate = 0.25;
  static constexpr double kMaxPlaybackRate = 4.0;

  // True when `rate` is a finite value inside the supported range. Shared by
  // the controller's protocol boundary (parse at the boundary), the player's
  // own setter (final guard) and LyricClock::setRate (deepest guard: direct
  // clock callers bypass both outer layers).
  static bool isValidPlaybackRate(double rate);

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
  // Test accessor: the active playback rate (1.0 until a valid rate is set;
  // invalid setPlaybackRate calls leave it unchanged).
  double playbackRate() const { return m_rate; }
  // Raw live re-anchor support (setOffset/setPlaybackRate/setVertical and
  // the controller's selector re-selection): the clock anchors the raw
  // caller position independently of the offset-inclusive position that
  // drives line selection, so a position captured BEFORE an
  // offset/lyric/rate change can be re-anchored through play() with the NEW
  // offset exactly once — otherwise the offset is applied twice and the
  // lyric jumps. Recoverable continuously while playing, even when the
  // inclusive anchor saturated (a subtractive reconstruction would be bogus
  // there). Empty when paused: a paused player keeps its position and only
  // re-anchors on the next explicit play().
  [[nodiscard]] std::optional<qint64> rawLivePositionMs() const;

signals:
  void lineChanged(int line, const QString& text);
  void lyricsChanged();

private:
  void refresh();
  int findCurLineNum(qint64 curTime, int startIndex = 0) const;
  void scheduleNext(qint64 delayMs);
  // Saturated composition of the total offset: [offset:] tag + user offset +
  // the 60 ms line-mode lead-in. Saturating so extreme tags/offsets (already
  // clamped to qint64 bounds by the parser) can never wrap into an
  // opposite-sign offset.
  void recomputeTotalOffset();
  // Delay from the live clock to targetTimeMs in whole milliseconds, clamped
  // to the QTimer-supported int range: the gap subtraction saturates and the
  // result is bounded BEFORE any narrowing conversion, so a pathological gap
  // (min-offset lyric, multi-hour line timestamp) schedules a finite
  // far-future timer instead of an overflowed interval or a 0 ms spin.
  // Returns 0 when the target is already due — callers treat that as "re-
  // anchor to the clock" rather than scheduling.
  qint64 lineDelayMs(qint64 targetTimeMs) const;

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
