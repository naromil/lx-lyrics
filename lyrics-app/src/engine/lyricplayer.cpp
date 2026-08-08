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

#include "engine/wordparser.h"

#include <QtGlobal>

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
    // Line mode reserves the first 60 ms for the un-timed lead-in word, exactly
    // like the Lyric facade in index.js.
    m_totalOffsetMs = m_tagOffsetMs + m_userOffsetMs + (m_lineMode ? 60 : 0);
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
    m_clock.resync(positionMs + m_totalOffsetMs);
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
    m_userOffsetMs = offsetMs;
    m_totalOffsetMs = m_tagOffsetMs + m_userOffsetMs + (m_lineMode ? 60 : 0);
    if (m_playing) {
        const qint64 pos = m_clock.currentPositionMs();
        pause();
        play(pos);
    }
}

void LyricPlayer::setPlaybackRate(double rate)
{
    m_rate = rate;
    m_clock.setRate(rate);
    if (m_playing) {
        const qint64 pos = m_clock.currentPositionMs();
        pause();
        play(pos);
    }
}

void LyricPlayer::setVertical(bool isVertical)
{
    Q_UNUSED(isVertical);
    if (m_playing) {
        const qint64 pos = m_clock.currentPositionMs();
        pause();
        play(pos);
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
    return (m_curLineNum >= 0 && m_curLineNum < m_lines.size()) ? m_lines[m_curLineNum].text : QString();
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
    const qint64 driftTimeMs = currentTime - curLine.timeMs;

    if (driftTimeMs >= 0) {
        const qint64 delayToNextLineMs = (m_lines[m_curLineNum + 1].timeMs - currentTime) / m_rate;
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
        scheduleNext((m_lines[m_firstTimedIndex].timeMs - currentTime) / m_rate);
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
    m_timer.start(qMax<qint64>(0, delayMs));
}

void LyricPlayer::onTimerTimeout()
{
    if (!m_playing)
        return;
    // refresh() re-anchors to the clock and skips any overshot lines itself,
    // exactly like the JS timeout callback calling _refresh().
    refresh();
}
