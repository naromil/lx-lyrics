/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
// Timed LRC line player: faithful C++23 port of LinePlayer from
// references/src/common/utils/lyric-font-player/line-player.js, driven by a
// LyricClock instead of getNow()/setTimeout. The Lyric facade's 60ms line-mode
// lead-in is folded into m_totalOffsetMs (see setLyric), and the timer handler
// calls refresh() directly exactly like the JS timeout callback.
//
// Remaining deviation from the JS reference: QTimer schedules whole
// milliseconds, so a next-line delay with a fractional part is truncated (JS
// setTimeout accepts fractional ms). With millisecond timestamps and the 60ms
// line-mode offset this never changes an emission.

#include "engine/lyricplayer.h"

#include "engine/checkedarith.h"
#include "engine/wordparser.h"

#include <QtGlobal>

#include <cmath>
#include <limits>

LyricPlayer::LyricPlayer(QObject* parent)
  : QObject(parent)
{
  m_timer.setSingleShot(true);
  m_timer.setParent(this);
  connect(&m_timer, &QTimer::timeout, this, &LyricPlayer::onTimerTimeout);
}

bool LyricPlayer::setLyric(const QString& lrc, const QStringList& extendedLyrics)
{
  // Idempotence guard against the host's periodic re-push of the current
  // track (deviation from line-player.js, justified: identical input -> no
  // state change -> no signal). Returns false for the dedup'd no-op and true
  // once the lyric was re-parsed and lyricsChanged signalled, so callers can
  // tell a real change from an identical re-push (the controller only
  // resumes after a real change). The reference re-runs setLyric
  // unconditionally, which pauses, resets the line to 0 and re-emits
  // lyricsChanged — a visible re-roll of the lyric list on every re-push.
  if (lrc == m_lastLrc && extendedLyrics == m_lastExtendedLyrics)
    return false;
  m_lastLrc = lrc;
  m_lastExtendedLyrics = extendedLyrics;

  if (m_playing)
    pause();

  const LrcParser::Result result = LrcParser::parse(lrc, extendedLyrics);
  m_lines = result.lines;
  m_firstTimedIndex = -1;
  for (int i = 0; i < m_lines.size(); ++i) {
    if (!m_lines[i].isStatic) {
      m_firstTimedIndex = i;
      break;
    }
  }
  // Line-mode detection uses the first TIMED line: a static lead line
  // (broken timestamp) must not flip a karaoke lyric into line mode.
  m_lineMode = m_firstTimedIndex >= 0 && !WordParser::hasTimeTags(m_lines[m_firstTimedIndex].text);
  m_tagOffsetMs = result.tag.offsetMs;
  recomputeTotalOffset();
  m_curLineNum = 0;
  emit lyricsChanged();
  return true;
}

void LyricPlayer::play(qint64 positionMs)
{
  // All-static lyrics never play: with no timed line nothing can ever become
  // active, so the static lines are never visited.
  if (m_lines.isEmpty() || m_firstTimedIndex < 0)
    return;
  pause();
  m_playing = true;
  // The clock anchors BOTH the raw caller position and the offset-inclusive
  // position (saturated composition): line selection runs on the inclusive
  // side, while the raw side is what a later live re-anchor recovers. The
  // raw position is stored independently — never reconstructed by
  // subtracting the offset, which is not invertible once the inclusive
  // composition saturates.
  m_clock.resync(positionMs, m_totalOffsetMs);
  m_curLineNum = findCurLineNum(m_clock.currentPositionMs()) - 1;
  refresh();
}

void LyricPlayer::pause()
{
  if (!m_playing)
    return;
  m_playing = false;
  m_timer.stop();
  // JS: at the max line the end was just emitted, so never re-derive from
  // the clock (at exactly the last line's time findCurLineNum would point
  // at the penultimate line).
  if (m_curLineNum == m_lines.size() - 1)
    return;
  const int found = findCurLineNum(m_clock.currentPositionMs());
  if (found != m_curLineNum && found >= 0 && found < m_lines.size()) {
    m_curLineNum = found;
    emit lineChanged(found, m_lines[found].text);
  }
}

void LyricPlayer::stop()
{
  pause();
  setLyric(QString());
  emit lineChanged(-1, QString());
}

void LyricPlayer::setOffset(qint64 offsetMs)
{
  // Capture the raw caller position BEFORE the offset changes: play()
  // applies the NEW total offset to the raw position exactly once —
  // replaying the offset-inclusive clock position would apply the offset
  // a second time.
  const std::optional<qint64> rawPositionMs = rawLivePositionMs();
  m_userOffsetMs = offsetMs;
  recomputeTotalOffset();
  if (rawPositionMs.has_value()) {
    pause();
    play(*rawPositionMs);
  }
}

void LyricPlayer::setPlaybackRate(double rate)
{
  // Final defense-in-depth guard (the controller's protocol boundary already
  // validated, but the player owns the timing math): an invalid rate must
  // never replace a valid current rate — zero/negative rates divide the
  // next-line delay by zero, and non-finite or out-of-range values can
  // overflow the qint64 conversions into a 0 ms timer spin. Rejected BEFORE
  // any capture, so the current rate AND playback position stay untouched.
  if (!isValidPlaybackRate(rate))
    return;
  // Same raw-position capture as setOffset, taken while the clock still runs
  // at the OLD rate: play() then re-anchors that raw position with the new
  // rate and the (unchanged) total offset exactly once, keeping the lyric
  // position continuous across the rate change.
  const std::optional<qint64> rawPositionMs = rawLivePositionMs();
  m_rate = rate;
  m_clock.setRate(rate);
  if (rawPositionMs.has_value()) {
    pause();
    play(*rawPositionMs);
  }
}

std::optional<qint64> LyricPlayer::rawLivePositionMs() const
{
  if (!m_playing)
    return std::nullopt;
  // The clock anchors the raw caller position independently of the
  // offset-inclusive position (see play()), so the raw position is
  // recoverable continuously — including when the inclusive anchor
  // saturated and a subtractive reconstruction (clock - total offset)
  // would return a bogus value. The raw channel advances with the SAME
  // elapsed time and playback rate as the inclusive position, and is only
  // ever empty here (paused): callers skip the re-anchor instead of
  // inventing a position.
  return m_clock.rawPositionMs();
}

void LyricPlayer::recomputeTotalOffset()
{
  // Line mode reserves the first 60 ms for the un-timed lead-in word, exactly
  // like the Lyric facade in index.js. Saturated composition: the tag and
  // user parts may each sit at the qint64 bounds, so the sum must clamp
  // instead of wrapping.
  m_totalOffsetMs =
    saturatingAdd(saturatingAdd(m_tagOffsetMs, m_userOffsetMs), m_lineMode ? 60 : 0);
}

qint64 LyricPlayer::lineDelayMs(qint64 targetTimeMs) const
{
  const qint64 gapMs = saturatingSub(targetTimeMs, m_clock.currentPositionMs());
  if (gapMs <= 0)
    return 0;
  // m_rate is validated in [kMinPlaybackRate, kMaxPlaybackRate] (never
  // zero), so the division is safe. The clamp BEFORE the double->qint64
  // conversion keeps the narrowing exact: an out-of-range conversion would
  // be UB, and an overflowed delay would schedule an invalid timer interval.
  const double delay = gapMs / m_rate;
  if (delay >= static_cast<double>(std::numeric_limits<int>::max()))
    return std::numeric_limits<int>::max();
  return static_cast<qint64>(delay);
}

bool LyricPlayer::isValidPlaybackRate(double rate)
{
  // Reject, never clamp: a host that sends a rate outside the supported
  // range must not silently distort the lyric clock. The range bounds the
  // delay math (division by m_rate, qint64 rounding) away from UB.
  return std::isfinite(rate) && rate >= kMinPlaybackRate && rate <= kMaxPlaybackRate;
}

void LyricPlayer::setVertical(bool isVertical)
{
  Q_UNUSED(isVertical);
  // Same raw re-anchor as setOffset/setPlaybackRate: the clock position
  // already includes the total offset and play() adds it again, so the RAW
  // position must cross the re-anchor. No production caller today (the
  // renderer owns vertical layout); kept correct through the shared helper.
  const std::optional<qint64> rawPositionMs = rawLivePositionMs();
  if (rawPositionMs.has_value()) {
    pause();
    play(*rawPositionMs);
  }
}

qint64 LyricPlayer::currentPositionMs() const
{
  return m_clock.currentPositionMs();
}

int LyricPlayer::currentLine() const
{
  // A static line is never "current" — this is what keeps the renderer from
  // activating a static line. Pre-play the player parks on index 0 (the
  // LEADING static of a mixed lyric, so the lead-in state reports -1 too);
  // after play() runs to the end it parks on the trailing index k-1, which
  // is always timed when playing (statics sort first), so it reports k-1.
  return (m_curLineNum >= 0 && m_curLineNum < m_lines.size() && !m_lines[m_curLineNum].isStatic)
           ? m_curLineNum
           : -1;
}

QString LyricPlayer::currentText() const
{
  // Intentional asymmetry with currentLine(): a parked static line reports
  // its text here but -1 from currentLine() (tests only; no production
  // caller).
  return (m_curLineNum >= 0 && m_curLineNum < m_lines.size()) ? m_lines[m_curLineNum].text
                                                              : QString();
}

bool LyricPlayer::isPlaying() const
{
  return m_playing;
}

const QVector<LrcLine>& LyricPlayer::lines() const
{
  return m_lines;
}

qint64 LyricPlayer::offset() const
{
  return m_totalOffsetMs;
}

int LyricPlayer::findCurLineNum(qint64 curTime, int startIndex) const
{
  // Statics are a prefix [0, m_firstTimedIndex) with timeMs == -1, so the
  // curTime <= timeMs test below never fires on them; the first timed line k
  // is line 0's analog — current even before its time. Behavior-identical
  // to the old "return 0" when there are no statics (k == 0).
  if (curTime <= 0)
    return m_firstTimedIndex;
  for (int i = startIndex; i < m_lines.size(); ++i)
    if (curTime <= m_lines[i].timeMs)
      return (i == m_firstTimedIndex ? i : i - 1);
  return m_lines.size() - 1;
}

void LyricPlayer::refresh()
{
  m_curLineNum += 1;
  if (m_curLineNum >= m_lines.size() - 1) {
    // JS _handleMaxLine: emit the last line once, then pause. Never reads
    // past the final line, so play() far past the end lands here safely.
    // The last line is always TIMED when playing (all-static lyrics never
    // reach play(), and statics sort first), so this never emits a static
    // index.
    emit lineChanged(m_lines.size() - 1, m_lines.last().text);
    pause();
    return;
  }

  const LrcLine& curLine = m_lines[m_curLineNum];
  const qint64 currentTime = m_clock.currentPositionMs();
  // Saturating: a boundary clock position (extreme offset) minus a line
  // time must not wrap into the opposite sign.
  const qint64 driftTimeMs = saturatingSub(currentTime, curLine.timeMs);

  if (driftTimeMs >= 0) {
    const qint64 delayToNextLineMs = lineDelayMs(m_lines[m_curLineNum + 1].timeMs);
    if (delayToNextLineMs > 0) {
      emit lineChanged(m_curLineNum, curLine.text);
      scheduleNext(delayToNextLineMs);
      return;
    }
    // The clock overshot the next line too: jump to the line the clock is
    // actually on, then refresh again (JS delay <= 0 branch).
    const int foundLineNum = findCurLineNum(currentTime, m_curLineNum + 1);
    if (foundLineNum > m_curLineNum)
      m_curLineNum = foundLineNum - 1;
    refresh();
    return;
  }

  if (m_curLineNum <= m_firstTimedIndex) {
    // Before the first timed line (static lines lead the list) the player
    // stays in the (-1,"") lead-in state and waits for that line.
    emit lineChanged(-1, QString());
    scheduleNext(lineDelayMs(m_lines[m_firstTimedIndex].timeMs));
    return;
  }

  // The timer fired before the next line's time: re-anchor to the line the
  // clock is actually on, then refresh again (JS drift < 0, curLineNum > 0).
  m_curLineNum = findCurLineNum(currentTime, m_curLineNum) - 1;
  refresh();
}

void LyricPlayer::scheduleNext(qint64 delayMs)
{
  if (!m_playing)
    return;
  // delayMs is already integral (the fractional part is dropped when the
  // call site converts the double), so no rounding is needed here.
  // QTimer::start() takes int milliseconds: clamp to the supported range so
  // an extreme (or negative) delay can never narrow into an invalid timer
  // interval.
  const qint64 clamped = std::clamp(delayMs, qint64(0), qint64(std::numeric_limits<int>::max()));
  m_timer.start(static_cast<int>(clamped));
}

void LyricPlayer::onTimerTimeout()
{
  if (!m_playing)
    return;
  // refresh() re-anchors to the clock and skips any overshot lines itself,
  // exactly like the JS timeout callback calling _refresh().
  refresh();
}
