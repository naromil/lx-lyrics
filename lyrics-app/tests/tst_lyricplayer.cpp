/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QPair>
#include <QSignalSpy>
#include <QTest>
#include <QVector>

#include <limits>
#include <optional>

#include "engine/checkedarith.h"
#include "engine/lyricclock.h"
#include "engine/lyricplayer.h"

// Tests for LyricPlayer (src/engine/lyricplayer.{h,cpp}), the timed LRC line
// player driving lineChanged/lyricsChanged from a monotonic clock.
//
// The port is faithful to the JS reference
// (references/src/common/utils/lyric-font-player/line-player.js) plus its
// Lyric facade (index.js):
// - Fractional timestamps are milliseconds: "[00:00.50]" is 50ms and
//   "[00:00.500]" is 500ms (JS reads the fractional digits as whole ms).
// - Line mode reserves 60ms of lead-in: total offset = [offset:] tag + user
//   offset + (lineMode ? 60 : 0). With kThreeLineLrc (first line at 50ms) the
//   clock is already past line 0 at play(0).
// - play(0) before a multi-line lyric's first line emits (-1,"") first, then
//   line 0 when the clock reaches it — the player never skips line 0.
// - Reaching the last line emits it once and pauses; play() far past the end
//   does the same without touching memory past the array.

namespace {

const QString kThreeLineLrc = QStringLiteral("[00:00.50]First\n"
                                             "[00:02.00]Second\n"
                                             "[00:03.50]Third");

// First line genuinely after the line-mode offset (500ms > 60ms) and with a
// comfortable gap before the last line, so the timer path runs deterministically.
const QString kFourLineLrc = QStringLiteral("[00:00.500]First\n"
                                            "[00:02.00]Second\n"
                                            "[00:05.00]Third\n"
                                            "[00:06.00]Fourth");

const QString kSingleLineFirst = QStringLiteral("[00:00.500]First");
const QString kLeadInLrc = QStringLiteral("[00:00.500]First\n"
                                          "[00:02.00]Second\n"
                                          "[00:03.50]Third");
const QString kOffsetLrc = QStringLiteral("[offset:500]\n[00:01.00]X");
const QString kRateLrc = QStringLiteral("[00:00.500]A\n"
                                        "[00:02.00]B\n"
                                        "[00:03.50]C");

// Multi-line lyric with a nonzero [offset:] tag and wide line spans, for the
// live re-anchor regressions: total offset = 500 (tag) + user offset + 60
// (line mode), so play(2000) with a 1000 ms user offset anchors the clock at
// 3560 and the spans keep the current line stable across the re-anchor.
const QString kTagOffsetMultiLineLrc = QStringLiteral("[offset:500]\n"
                                                      "[00:01.00]A\n"
                                                      "[00:03.00]B\n"
                                                      "[00:05.00]C");

// Karaoke (timed-word) lyric with NO [offset:] tag: total offset = user part
// only, no line-mode lead-in — the opposite selection from
// kTagOffsetMultiLineLrc, so swapping between them changes the total offset
// (the selector re-selection scenario).
const QString kKaraokeNoTagLrc = QStringLiteral("[00:01.00]<0,999>A\n"
                                                "[00:03.00]<0,999>B\n"
                                                "[00:05.00]<0,999>C");

// [offset:] tags whose digit runs overflow qint64: the parser saturates them
// to qint64::max()/min(), and the player's offset composition must saturate
// too instead of wrapping into the opposite sign.
const QString kHugePositiveOffsetLrc = QStringLiteral("[offset:99999999999999999999]\n"
                                                      "[00:01.00]A\n"
                                                      "[00:03.00]B\n"
                                                      "[00:05.00]C");
const QString kHugeNegativeOffsetLrc = QStringLiteral("[offset:-99999999999999999999]\n"
                                                      "[00:01.00]A\n"
                                                      "[00:03.00]B\n"
                                                      "[00:05.00]C");

// First line at 998:59:59.999 (3,596,399,999 ms) — beyond the QTimer int
// range — so the lead-in delay must clamp to the supported maximum instead
// of narrowing into an invalid (negative) timer interval.
const QString kHugeLineTimeLrc = QStringLiteral("[998:59:59.999]Huge\n"
                                                "[999:00:00.000]Later");

int indexOfLine(const QVector<QPair<int, QString>>& emissions, int line, const QString& text)
{
  for (int i = 0; i < emissions.size(); ++i)
    if (emissions.at(i).first == line && emissions.at(i).second == text)
      return i;
  return -1;
}

} // namespace

class TestLyricPlayer : public QObject {
  Q_OBJECT

private slots:
  // 1. setLyric emits lyricsChanged and lines() has parsed lines.
  void setLyricEmitsLyricsChangedAndParsesLines();
  // 2. Single-line lyric: the only line is emitted at once, then the player
  //    pauses (JS _handleMaxLine) — there is no (-1,"") lead-in.
  void playSingleLineEmitsOnlyLineImmediately();
  // 3. play(0) before the first line of a multi-line lyric: first signal is
  //    lineChanged(-1,""), then line 0 with its text after the line's time.
  void playBeforeFirstLineEmitsMinusOneThenLineZero();
  // 4. play() starting mid-lyric lands on line 0, then advances to line 1.
  void playMidLyricAdvancesToNextLine();
  // 5. play(0) before the first line never skips line 0: emissions arrive in
  //    order (-1,""), line 0 "First", line 1 "Second".
  void playFromZeroMultiLineEmitsLinesInOrder();
  // 6. play() far past the last line must not touch memory past the array:
  //    the last line is emitted and the player pauses.
  void playPastEndMultiLineDoesNotCrash();
  // 7. The multi-line end path: playing exactly on the last line's time, or
  //    slightly before it, ends paused with the last line active.
  void playAtLastLineEndsWithLastLineActive();
  // 8. pause() mid-play keeps the current text.
  void pauseMidPlayKeepsCurrentText();
  // 9. setPlaybackRate(2.0) roughly doubles progression.
  void setPlaybackRateDoublesProgression();
  // 10. Offset composition: [offset:] tag + user offset + 60ms line-mode.
  void offsetCompositionAddsTagUserAndLineMode();
  // 11. stop() clears the lyric and emits lineChanged(-1,"").
  void stopClearsLyricAndEmitsMinusOne();
  // 12. setVertical(true) while playing re-anchors and keeps playing.
  void setVerticalKeepsPlaying();
  // 13. play(0) with an empty lyric is a no-op.
  void playEmptyLyricIsNoOp();
  // 14. An identical setLyric re-push is deduped: no pause, no reset, no
  //     lyricsChanged. A changed body or extended list still emits.
  void setLyricTwiceEmitsOnce();
  // 15. Invalid playback rates (zero, negative, NaN/Infinity, out-of-range)
  //     are rejected and never replace a valid current rate.
  void setPlaybackRateRejectsInvalidRates();
  // 16. A rejected zero rate must not divide the delay math by zero or spin
  //     a 0 ms timer: playback keeps progressing at the retained rate.
  void invalidRateDoesNotSpinTimer();
  // 17. Live rate re-anchor with a nonzero total offset (tag + user + line
  //     mode): the clock position must stay continuous — the offset must not
  //     be applied a second time.
  void setPlaybackRateLiveReanchorKeepsPosition();
  // 18. Live offset re-anchor while playing: the RAW position must be kept —
  //     only the NEW total offset is applied, never the old one twice.
  void setOffsetLiveReanchorKeepsRawPosition();
  // 19. A rejected invalid rate while playing must leave the current rate,
  //     the playback position, and the playing state untouched.
  void invalidRateWhilePlayingLeavesPositionUntouched();
  // 20. Direct contract of the saturating qint64 helpers: exact boundary
  //     combinations clamp instead of invoking signed-overflow UB.
  void saturatingArithmeticBoundaries();
  // 21. Selector re-selection while playing (the controller's pattern):
  //     capture the RAW position, swap in a lyric with a DIFFERENT total
  //     offset, resume with the raw position — the new offset applies
  //     exactly once, no jump.
  void selectorReselectionRawReanchorKeepsPosition();
  // 22. setVertical() live re-anchor with a nonzero total offset: the clock
  //     position must stay continuous — the offset must not be applied a
  //     second time.
  void setVerticalLiveReanchorKeepsRawPosition();
  // 23. Extreme [offset:] tags (parser-saturated to the qint64 bounds)
  //     saturate the total-offset composition instead of wrapping into an
  //     opposite-sign offset.
  void extremeTagOffsetsSaturateTotalOffset();
  // 24. Playing with a saturated max total offset must not invoke UB and
  //     must not spin the timer: the clock sits far past the end, the last
  //     line is emitted once, and the clock stays saturated.
  void maxOffsetPlayEndsSafelyAtLastLine();
  // 25. Playing with a saturated min total offset parks in the lead-in with
  //     a far-future CLAMPED timer delay — never a 0 ms spin (the unchecked
  //     gap subtraction overflowed and scheduled an immediate timer).
  void minOffsetPlayParksLeadInWithClampedDelay();
  // 26. Positive saturated total offset, then live re-anchors: the raw
  //     position must never be replaced with the bogus subtractive
  //     reconstruction (max - max = 0) and playback must remain safe.
  void positiveSaturatedOffsetLiveReanchorKeepsRawPosition();
  // 27. Negative saturated total offset, then a live re-anchor: the raw
  //     channel must report the caller's raw position while playing (never
  //     the bogus min - (min + 60) = -60) and the re-anchor must keep
  //     playback safe.
  void negativeSaturatedOffsetLiveReanchorKeepsRawPosition();
  // 28. The raw-position contract at the clock level: the independent raw
  //     anchor survives BOTH saturation directions, advances with elapsed
  //     playback, and scales with the playback rate.
  void rawPositionRecoverySurvivesInclusiveSaturation();
  // 29. A line timestamp beyond the QTimer int range clamps the next-line
  //     delay to the supported maximum instead of narrowing into an invalid
  //     (negative) interval.
  void hugeLineTimeDelayClampsToTimerRange();
  // 30. Clock-level defense in depth: LyricClock::setRate itself rejects
  //     NaN, both infinities, zero, negatives, tiny and out-of-range rates
  //     — preserving the last valid rate and the anchored position even
  //     when a direct caller bypasses the player's validation.
  void clockSetRateRejectsInvalidRates();
  // 31. The rejection guard must not change valid-rate behavior: a valid
  //     clock rate still scales elapsed wall time, and a rejected rate
  //     leaves the clock advancing at the retained rate.
  void clockValidRateStillScalesElapsed();
};

void TestLyricPlayer::setLyricEmitsLyricsChangedAndParsesLines()
{
  LyricPlayer player;
  QSignalSpy lyricsSpy(&player, &LyricPlayer::lyricsChanged);

  player.setLyric(kThreeLineLrc);

  QCOMPARE(lyricsSpy.count(), 1);
  QCOMPARE(player.lines().size(), 3);
  // Fractional seconds are milliseconds: "[00:00.50]" -> 50ms, not 500ms.
  QCOMPARE(player.lines().at(0).timeMs, qint64(50));
  QCOMPARE(player.lines().at(0).text, QStringLiteral("First"));
  QCOMPARE(player.lines().at(1).timeMs, qint64(2000));
  QCOMPARE(player.lines().at(1).text, QStringLiteral("Second"));
  QCOMPARE(player.lines().at(2).timeMs, qint64(3050));
  QCOMPARE(player.lines().at(2).text, QStringLiteral("Third"));
}

void TestLyricPlayer::playSingleLineEmitsOnlyLineImmediately()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kSingleLineFirst); // one line at 500ms
  player.play(0);                    // a single line is already the max line

  // JS _handleMaxLine: the only line is emitted synchronously, then pause().
  QVERIFY(!player.isPlaying());
  QCOMPARE(player.currentLine(), 0);
  QCOMPARE(player.currentText(), QStringLiteral("First"));
  QCOMPARE(emissions.size(), 1);
  QCOMPARE(emissions.first().first, 0);
  QCOMPARE(emissions.first().second, QStringLiteral("First"));
}

void TestLyricPlayer::playBeforeFirstLineEmitsMinusOneThenLineZero()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kLeadInLrc); // first line at 500ms > 60ms offset
  player.play(0);              // synchronously before the first line

  QVERIFY(!emissions.empty());
  QCOMPARE(emissions.first().first, -1);
  QVERIFY(emissions.first().second.isEmpty());

  QTest::qWait(600); // 500ms line with +60ms offset -> ~440ms wall time

  const int firstIdx = indexOfLine(emissions, 0, QStringLiteral("First"));
  QVERIFY(firstIdx > 0); // the (-1,"") lead-in precedes line 0
  QCOMPARE(player.currentText(), QStringLiteral("First"));
}

void TestLyricPlayer::playMidLyricAdvancesToNextLine()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kFourLineLrc); // lines at 500 / 2000 / 5000 / 6000
  player.play(1500);             // 1500+60 lands inside line 0's span

  QVERIFY(!emissions.empty());
  QCOMPARE(emissions.first().first, 0);
  QCOMPARE(emissions.first().second, QStringLiteral("First"));
  QCOMPARE(player.currentLine(), 0);
  QCOMPARE(player.currentText(), QStringLiteral("First"));

  QTest::qWait(600); // passes 2000ms; line 1 "Second" must have been emitted

  QVERIFY(indexOfLine(emissions, 1, QStringLiteral("Second")) >= 0);
  // pause() re-derives the current line from the clock, so the final state
  // is deterministic regardless of timer timing.
  player.pause();
  QCOMPARE(player.currentLine(), 1);
  QCOMPARE(player.currentText(), QStringLiteral("Second"));
}

void TestLyricPlayer::playFromZeroMultiLineEmitsLinesInOrder()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kLeadInLrc); // lines at 500 / 2000 / 3050 ms
  player.play(0);

  QCOMPARE(emissions.first().first, -1);
  QVERIFY(emissions.first().second.isEmpty());

  QTest::qWait(2200); // passes 500ms (line 0) and 2000ms (line 1)

  // The player never skips line 0: (-1,"") -> (0,"First") -> (1,"Second").
  const int minusOneIdx = indexOfLine(emissions, -1, QString());
  const int firstIdx = indexOfLine(emissions, 0, QStringLiteral("First"));
  const int secondIdx = indexOfLine(emissions, 1, QStringLiteral("Second"));
  QVERIFY(minusOneIdx >= 0);
  QVERIFY(firstIdx > minusOneIdx);
  QVERIFY(secondIdx > firstIdx);
}

void TestLyricPlayer::playPastEndMultiLineDoesNotCrash()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kLeadInLrc);
  player.play(100000); // far past the last line; must not read out of bounds

  // The last line is emitted once and the player pauses (JS _handleMaxLine).
  QVERIFY(!player.isPlaying());
  QCOMPARE(player.currentLine(), 2);
  QCOMPARE(player.currentText(), QStringLiteral("Third"));
  QCOMPARE(emissions.size(), 1);
  QCOMPARE(emissions.last().first, 2);
  QCOMPARE(emissions.last().second, QStringLiteral("Third"));
}

void TestLyricPlayer::playAtLastLineEndsWithLastLineActive()
{
  LyricPlayer exactPlayer;
  QVector<QPair<int, QString>> exactEmissions;
  QObject::connect(&exactPlayer, &LyricPlayer::lineChanged, &exactPlayer,
                   [&exactEmissions](int line, const QString& text) {
                     exactEmissions.append({line, text});
                   });

  exactPlayer.setLyric(kLeadInLrc); // last line at 3050ms
  exactPlayer.play(2990);           // clock 3050 == exactly the last line time

  QVERIFY(!exactPlayer.isPlaying());
  QCOMPARE(exactPlayer.currentLine(), 2);
  QCOMPARE(exactPlayer.currentText(), QStringLiteral("Third"));

  // Slightly before the last line: line 1 stays active until the timer
  // advances to the last line, then the player pauses.
  LyricPlayer nearPlayer;
  QVector<QPair<int, QString>> nearEmissions;
  QObject::connect(&nearPlayer, &LyricPlayer::lineChanged, &nearPlayer,
                   [&nearEmissions](int line, const QString& text) {
                     nearEmissions.append({line, text});
                   });

  nearPlayer.setLyric(kLeadInLrc);
  nearPlayer.play(2500); // clock 2560: inside line 1's span

  QVERIFY(nearPlayer.isPlaying());
  QCOMPARE(nearPlayer.currentLine(), 1);
  QCOMPARE(nearPlayer.currentText(), QStringLiteral("Second"));

  QTest::qWait(600); // passes 3050ms; last line emitted and playing stops

  QVERIFY(!nearPlayer.isPlaying());
  QCOMPARE(nearPlayer.currentLine(), 2);
  QCOMPARE(nearPlayer.currentText(), QStringLiteral("Third"));
}

void TestLyricPlayer::pauseMidPlayKeepsCurrentText()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kThreeLineLrc); // first line at 50ms < 60ms offset
  player.play(0);                 // line 0 is active immediately

  QCOMPARE(player.currentLine(), 0);
  QCOMPARE(player.currentText(), QStringLiteral("First"));

  player.pause();

  QVERIFY(!player.isPlaying());
  QCOMPARE(player.currentText(), QStringLiteral("First"));
  QCOMPARE(emissions.size(), 1); // pause() emitted nothing extra
}

void TestLyricPlayer::setPlaybackRateDoublesProgression()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kRateLrc); // first line at 500ms
  player.setPlaybackRate(2.0);
  player.play(0); // before first line: (-1,"") synchronously

  QVERIFY(!emissions.empty());
  QCOMPARE(emissions.first().first, -1);

  // At rate 2.0 the 500ms line is reached after ~220ms of wall time, well
  // inside a 400ms wait (the rate-1.0 delay would be ~440ms).
  QTest::qWait(400);
  QVERIFY(indexOfLine(emissions, 0, QStringLiteral("A")) >= 0);

  // Contrast at rate 1.0: the same line is not reached within 350ms
  // (~440ms needed), showing the doubled progression is real.
  LyricPlayer slowPlayer;
  QVector<QPair<int, QString>> slowEmissions;
  QObject::connect(&slowPlayer, &LyricPlayer::lineChanged, &slowPlayer,
                   [&slowEmissions](int line, const QString& text) {
                     slowEmissions.append({line, text});
                   });
  slowPlayer.setLyric(kRateLrc);
  slowPlayer.play(0);
  QTest::qWait(350);
  QCOMPARE(indexOfLine(slowEmissions, 0, QStringLiteral("A")), -1);
}

void TestLyricPlayer::offsetCompositionAddsTagUserAndLineMode()
{
  LyricPlayer player;

  // "X" has no karaoke tags, so the lyric is in line mode (+60ms).
  player.setLyric(kOffsetLrc);
  QCOMPARE(player.lines().size(), 1);

  player.setOffset(200);

  // 500 ([offset:]) + 200 (user) + 60 (line-mode) = 760.
  QCOMPARE(player.offset(), qint64(760));
}

void TestLyricPlayer::stopClearsLyricAndEmitsMinusOne()
{
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kThreeLineLrc);
  player.play(1500); // emits (0,"First") synchronously
  QVERIFY(!emissions.empty());

  player.stop();

  QVERIFY(!player.isPlaying());
  QVERIFY(player.lines().isEmpty());
  QVERIFY(player.currentText().isEmpty());
  QCOMPARE(emissions.last().first, -1);
  QVERIFY(emissions.last().second.isEmpty());
}

void TestLyricPlayer::setVerticalKeepsPlaying()
{
  LyricPlayer player;

  player.setLyric(kThreeLineLrc);
  player.play(1500);
  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 0);

  player.setVertical(true); // re-anchor: pause() + play(currentPositionMs)

  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 0);

  QTest::qWait(150); // timer for the next line is not due yet (~440ms)
  QVERIFY(player.isPlaying());
}

void TestLyricPlayer::playEmptyLyricIsNoOp()
{
  LyricPlayer player; // never given a lyric: lines() is empty
  QSignalSpy lineSpy(&player, &LyricPlayer::lineChanged);
  QSignalSpy lyricSpy(&player, &LyricPlayer::lyricsChanged);

  player.play(0);

  QVERIFY(!player.isPlaying());
  QTest::qWait(150);
  QCOMPARE(lineSpy.count(), 0);
  QCOMPARE(lyricSpy.count(), 0);
}

void TestLyricPlayer::setLyricTwiceEmitsOnce()
{
  LyricPlayer player;
  QSignalSpy lyricSpy(&player, &LyricPlayer::lyricsChanged);

  // The first setLyric always emits.
  player.setLyric(kThreeLineLrc);
  QCOMPARE(lyricSpy.count(), 1);

  // An identical re-push (the host's periodic set_info+set_lyric+set_play)
  // must not pause/reset/re-emit: same input -> no state change -> no signal.
  player.setLyric(kThreeLineLrc);
  QCOMPARE(lyricSpy.count(), 1);

  // A change in the extended lyrics is a real change, not deduped.
  player.setLyric(kThreeLineLrc, QStringList{QStringLiteral("Extra translation")});
  QCOMPARE(lyricSpy.count(), 2);

  // The identical extended list dedupes too.
  player.setLyric(kThreeLineLrc, QStringList{QStringLiteral("Extra translation")});
  QCOMPARE(lyricSpy.count(), 2);

  // A changed lyric body is a real change, not deduped.
  player.setLyric(kThreeLineLrc + QStringLiteral("\n"));
  QCOMPARE(lyricSpy.count(), 3);

  // A playing player survives an identical re-push: the dedup guard returns
  // before pause(), so playback continues (pre-dedup-fix this paused and
  // re-emitted the current line).
  const QStringList extended{QStringLiteral("Extra translation")};
  player.setLyric(kFourLineLrc, extended);
  QCOMPARE(lyricSpy.count(), 4);
  player.play(1000); // mid-lyric: line 0 active, next-line timer scheduled
  QVERIFY(player.isPlaying());

  player.setLyric(kFourLineLrc, extended); // identical: dedup no-op
  QVERIFY(player.isPlaying());             // still playing — the re-push did not pause
  QCOMPARE(lyricSpy.count(), 4);

  // A real change pauses the player (setLyric pauses before re-parsing).
  player.setLyric(kFourLineLrc + QStringLiteral("\n"), extended);
  QVERIFY(!player.isPlaying());
  QCOMPARE(lyricSpy.count(), 5);

  // The bool contract the controller depends on: true once the lyric was
  // re-parsed and lyricsChanged signalled, false for the identical re-push
  // no-op. A fresh player keeps the first call unambiguous.
  LyricPlayer freshPlayer;
  QSignalSpy freshSpy(&freshPlayer, &LyricPlayer::lyricsChanged);
  QVERIFY(freshPlayer.setLyric(kThreeLineLrc)); // fresh lyric -> true
  QCOMPARE(freshSpy.count(), 1);
  QVERIFY(!freshPlayer.setLyric(kThreeLineLrc)); // same lrc + same extended -> false
  QCOMPARE(freshSpy.count(), 1);
  QVERIFY(freshPlayer.setLyric(kThreeLineLrc + QStringLiteral("\n"))); // changed -> true
  QCOMPARE(freshSpy.count(), 2);
}

void TestLyricPlayer::setPlaybackRateRejectsInvalidRates()
{
  LyricPlayer player;
  player.setLyric(kRateLrc);
  player.setPlaybackRate(2.0);
  QCOMPARE(player.playbackRate(), 2.0);

  // Zero, negatives, NaN, Infinity and out-of-range values must NOT replace
  // the valid current rate (defense in depth: a 0 rate would divide the
  // next-line delay by zero; non-finite values overflow the qint64 math).
  player.setPlaybackRate(0.0);
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(-1.0);
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(-0.5);
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(std::numeric_limits<double>::quiet_NaN());
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(std::numeric_limits<double>::infinity());
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(-std::numeric_limits<double>::infinity());
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(100.0); // Above kMaxPlaybackRate.
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(0.01); // Below kMinPlaybackRate.
  QCOMPARE(player.playbackRate(), 2.0);

  // Valid in-range rates still apply; the boundary values themselves work.
  player.setPlaybackRate(LyricPlayer::kMinPlaybackRate);
  QCOMPARE(player.playbackRate(), LyricPlayer::kMinPlaybackRate);
  player.setPlaybackRate(LyricPlayer::kMaxPlaybackRate);
  QCOMPARE(player.playbackRate(), LyricPlayer::kMaxPlaybackRate);
  player.setPlaybackRate(0.5);
  QCOMPARE(player.playbackRate(), 0.5);
}

void TestLyricPlayer::invalidRateDoesNotSpinTimer()
{
  // Regression: a rate <= 0 previously divided by zero / overflowed into a
  // qint64 and could schedule a 0 ms timer spin (scheduleNext clamps with
  // qMax(0, delay) after an UB conversion). The rejected rate must leave the
  // valid rate in place, so the player keeps progressing normally.
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kRateLrc); // first line at 500ms
  player.setPlaybackRate(2.0);
  player.play(0); // before the first line: (-1,"") synchronously
  QVERIFY(!emissions.empty());

  // Attempt the spin-inducing rate mid-play; it must be rejected.
  player.setPlaybackRate(0.0);
  QCOMPARE(player.playbackRate(), 2.0);
  player.setPlaybackRate(std::numeric_limits<double>::quiet_NaN());
  QCOMPARE(player.playbackRate(), 2.0);

  // At the retained 2.0 rate the 500ms line is reached after ~220ms of wall
  // time, well inside a 400ms wait. A 0ms spin would either hang (infinite
  // 0-delay timer chain) or never emit the line.
  QTest::qWait(400);
  QVERIFY(indexOfLine(emissions, 0, QStringLiteral("A")) >= 0);
  QVERIFY(player.isPlaying()); // Still running toward line 1 at 2000ms.
  QCOMPARE(player.playbackRate(), 2.0);
}

void TestLyricPlayer::setPlaybackRateLiveReanchorKeepsPosition()
{
  // REGRESSION (double-offset): setPlaybackRate captured the clock position
  // (which ALREADY includes the total offset — play() anchored it with
  // resync(pos + m_totalOffsetMs)) and re-anchored through play(), which
  // added the offset AGAIN. With a 1560 ms total offset the lyric jumped a
  // full offset forward on every rate change (and with this fixture, jumped
  // straight to the last line and paused). The re-anchor must keep the
  // position continuous across the rate change.
  LyricPlayer player;
  player.setLyric(kTagOffsetMultiLineLrc);
  player.setOffset(1000); // Total offset: 500 (tag) + 1000 (user) + 60 = 1560.
  QCOMPARE(player.offset(), qint64(1560));

  player.play(2000); // Clock anchors at 3560: inside line 1's span (3000-5000).
  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);
  QCOMPARE(player.currentText(), QStringLiteral("B"));

  const qint64 posBefore = player.currentPositionMs();
  player.setPlaybackRate(2.0);

  // No extra jump: the position after the re-anchor is the same (the tiny
  // elapsed time between the two reads, plus resync quantization, is far
  // below the 100 ms tolerance; the buggy double-application jumped 1560 ms).
  QVERIFY(player.isPlaying());
  QCOMPARE(player.playbackRate(), 2.0);
  QCOMPARE(player.currentLine(), 1);
  QVERIFY(qAbs(player.currentPositionMs() - posBefore) < 100);
}

void TestLyricPlayer::setOffsetLiveReanchorKeepsRawPosition()
{
  // REGRESSION (double-offset, live setOffset path): changing the user
  // offset while playing used the offset-inclusive clock position and then
  // play() applied the NEW total offset on top of it — the position gained
  // the OLD total offset on every adjustment. The raw caller position must be
  // preserved: only the NEW total offset applies, exactly once.
  LyricPlayer player;
  player.setLyric(kTagOffsetMultiLineLrc);
  player.setOffset(1000); // Total offset: 500 (tag) + 1000 (user) + 60 = 1560.
  player.play(2000);      // Clock anchors at 3560: inside line 1's span.

  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);

  player.setOffset(2000); // New total offset: 500 + 2000 + 60 = 2560.

  // Raw position was 2000 (modulo the sub-ms elapsed since play()); the
  // re-anchored clock is raw + 2560 = 4560, still inside line 1's span.
  // The buggy double-application anchored at clock + 2560 = 6120 (past the
  // last line, which paused the player).
  QCOMPARE(player.offset(), qint64(2560));
  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);
  QCOMPARE(player.currentText(), QStringLiteral("B"));
  QVERIFY(qAbs(player.currentPositionMs() - 4560) < 100);
}

void TestLyricPlayer::invalidRateWhilePlayingLeavesPositionUntouched()
{
  // The invalid-rate guard must reject BEFORE any position capture or
  // re-anchor: the current valid rate, the playback position, the playing
  // state and the active line all stay exactly as they were.
  LyricPlayer player;
  player.setLyric(kTagOffsetMultiLineLrc);
  player.setOffset(1000); // Total offset: 1560.
  player.play(2000);      // Clock anchors at 3560: inside line 1's span.

  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);

  const qint64 posBefore = player.currentPositionMs();
  player.setPlaybackRate(0.0);  // Rejected (division by zero).
  player.setPlaybackRate(42.0); // Rejected (out of range).

  QCOMPARE(player.playbackRate(), 1.0);
  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);
  QVERIFY(qAbs(player.currentPositionMs() - posBefore) < 100);
}

void TestLyricPlayer::saturatingArithmeticBoundaries()
{
  const qint64 max = std::numeric_limits<qint64>::max();
  const qint64 min = std::numeric_limits<qint64>::min();

  // Addition clamps instead of wrapping.
  QCOMPARE(saturatingAdd(max, 1), max);
  QCOMPARE(saturatingAdd(max, max), max);
  QCOMPARE(saturatingAdd(min, -1), min);
  QCOMPARE(saturatingAdd(min, min), min);
  QCOMPARE(saturatingAdd(max, 0), max);
  QCOMPARE(saturatingAdd(min, 0), min);
  QCOMPARE(saturatingAdd(max, min), qint64(-1)); // Representable: no clamp.

  // Subtraction clamps instead of wrapping (max - min would overflow).
  QCOMPARE(saturatingSub(min, 1), min);
  QCOMPARE(saturatingSub(min, max), min);
  QCOMPARE(saturatingSub(max, -1), max);
  QCOMPARE(saturatingSub(max, min), max);
  QCOMPARE(saturatingSub(min, 0), min);
  QCOMPARE(saturatingSub(-5, 10), qint64(-15));
}

void TestLyricPlayer::selectorReselectionRawReanchorKeepsPosition()
{
  // REGRESSION (double-offset, selector re-selection): the controller's
  // pattern for a selector-key change (translation/roma/LX lyric) is
  // capture -> re-select -> resume. The re-selected lyric can carry a
  // DIFFERENT total offset (here: tag 500 + line mode 60 vs karaoke with no
  // tag), so the clock position captured BEFORE the swap — which already
  // includes the OLD offset — must NOT be fed back through play() (that
  // applies the offset twice). The raw position captured via the dedicated
  // API must cross the re-anchor instead.
  LyricPlayer player;
  player.setLyric(kTagOffsetMultiLineLrc);
  player.setOffset(1000); // Total offset: 500 (tag) + 1000 (user) + 60 = 1560.
  player.play(2000);      // Clock anchors at 3560: inside line 1's span.

  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);

  // The controller's capture: the RAW caller position (clock minus OLD
  // total offset), never the offset-inclusive clock position.
  const std::optional<qint64> raw = player.rawLivePositionMs();
  QVERIFY(raw.has_value());
  QVERIFY(qAbs(*raw - 2000) < 100);

  // The controller's re-apply: a different lyric with a different total
  // offset. setLyric pauses, exactly like the selector re-selection.
  player.setLyric(kKaraokeNoTagLrc);       // Karaoke: no tag, no line mode.
  QCOMPARE(player.offset(), qint64(1000)); // 0 + 1000 + 0.
  QVERIFY(!player.isPlaying());

  // Resume with the RAW position: the NEW offset applies exactly once. The
  // buggy double-application anchored at old clock (3560) + new offset
  // (1000) = 4560 — a 1560 ms jump.
  player.play(*raw);
  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);
  QCOMPARE(player.currentText(), QStringLiteral("<0,999>B")); // Karaoke lines keep their tags.
  QVERIFY(qAbs(player.currentPositionMs() - 3000) < 100);
}

void TestLyricPlayer::setVerticalLiveReanchorKeepsRawPosition()
{
  // REGRESSION (double-offset, setVertical path): setVertical captured the
  // offset-INCLUSIVE clock position and re-anchored through play(), adding
  // the total offset a second time. With a 1560 ms total offset the lyric
  // jumped past the last line and paused. The shared raw helper keeps the
  // position continuous. (No production caller today; the fix goes through
  // the same helper as setOffset/setPlaybackRate.)
  LyricPlayer player;
  player.setLyric(kTagOffsetMultiLineLrc);
  player.setOffset(1000); // Total offset: 1560.
  player.play(2000);      // Clock anchors at 3560: inside line 1's span.

  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);

  const qint64 posBefore = player.currentPositionMs();
  player.setVertical(true);

  QVERIFY(player.isPlaying());
  QCOMPARE(player.currentLine(), 1);
  QCOMPARE(player.currentText(), QStringLiteral("B"));
  QVERIFY(qAbs(player.currentPositionMs() - posBefore) < 100);
}

void TestLyricPlayer::extremeTagOffsetsSaturateTotalOffset()
{
  // [offset:] overflow saturates in the parser to qint64::max(); the total
  // offset composition (tag + user + 60 line mode) must saturate too — an
  // unchecked add would wrap into an opposite-sign offset.
  LyricPlayer player;
  player.setLyric(kHugePositiveOffsetLrc);
  QCOMPARE(player.offset(), std::numeric_limits<qint64>::max()); // max + 0 + 60.

  player.setOffset(std::numeric_limits<qint64>::max());
  QCOMPARE(player.offset(), std::numeric_limits<qint64>::max()); // max + max.

  player.setOffset(std::numeric_limits<qint64>::min());
  // max + min = -1 is representable, so only the line-mode 60 is added.
  QCOMPARE(player.offset(), qint64(59));

  LyricPlayer negative;
  negative.setLyric(kHugeNegativeOffsetLrc); // min + 0 + 60 = min + 60.
  QCOMPARE(negative.offset(), std::numeric_limits<qint64>::min() + 60);

  negative.setOffset(std::numeric_limits<qint64>::min()); // min + min -> min;
                                                          // + 60 stays representable.
  QCOMPARE(negative.offset(), std::numeric_limits<qint64>::min() + 60);
}

void TestLyricPlayer::maxOffsetPlayEndsSafelyAtLastLine()
{
  // REGRESSION (overflow, play anchor): resync(position + total offset) with
  // a saturated max offset used to overflow into a wrapped (negative) clock.
  // The saturated anchor keeps the clock far past the end: the last line is
  // emitted once, the player pauses, and the clock stays at qint64::max()
  // instead of wrapping as time advances.
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kHugePositiveOffsetLrc);
  player.play(1000); // 1000 + max saturates to max: far past the last line.

  QVERIFY(!player.isPlaying());
  QCOMPARE(player.currentLine(), 2);
  QCOMPARE(player.currentText(), QStringLiteral("C"));
  QCOMPARE(emissions.size(), 1);
  QCOMPARE(emissions.last().first, 2);
  QCOMPARE(emissions.last().second, QStringLiteral("C"));

  // The clock saturates instead of wrapping as elapsed time accrues.
  QCOMPARE(player.currentPositionMs(), std::numeric_limits<qint64>::max());
  QTest::qWait(50);
  QCOMPARE(player.currentPositionMs(), std::numeric_limits<qint64>::max());
  QCOMPARE(emissions.size(), 1); // No timer spin after the end emission.
}

void TestLyricPlayer::minOffsetPlayParksLeadInWithClampedDelay()
{
  // REGRESSION (overflow, raw recovery + delay): with a saturated min offset
  // the clock anchors near qint64::min(). The unchecked gap subtraction
  // (line time - clock) overflowed into a negative delay, which qMax(0, ...)
  // turned into a 0 ms timer spin (infinite refresh recursion). The
  // saturating gap now clamps to the QTimer int maximum: a far-future timer,
  // exactly one (-1,"") lead-in emission, never a spin.
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kHugeNegativeOffsetLrc); // Total offset: min + 60.
  player.play(0);                          // Clock anchored at min + 60.

  QVERIFY(player.isPlaying()); // Parked in the lead-in, waiting for line 0.
  QCOMPARE(emissions.size(), 1);
  QCOMPARE(emissions.first().first, -1);
  QVERIFY(emissions.first().second.isEmpty());

  QTest::qWait(100); // A 0 ms spin would re-emit/loop; the clamped far-future
                     // delay keeps the lead-in silent.
  QVERIFY(player.isPlaying());
  QCOMPARE(emissions.size(), 1);
}

void TestLyricPlayer::positiveSaturatedOffsetLiveReanchorKeepsRawPosition()
{
  // REGRESSION (raw recovery, positive saturation): a raw position of 1000
  // with a saturated max total offset anchors the INCLUSIVE clock at
  // qint64::max(), and the old subtractive reconstruction (clock - offset)
  // returned the bogus 0 there (max - max). The raw position must never be
  // replaced with that zero: while the player is parked the raw channel is
  // EMPTY (a paused player only re-anchors on the next explicit play()), so
  // a live setPlaybackRate/setOffset skips the re-anchor and the clock
  // stays at max — playback remains safe instead of jumping to the start.
  LyricPlayer player;
  player.setLyric(kHugePositiveOffsetLrc); // Total offset saturates at max.
  player.play(1000);                       // Inclusive anchor saturates at max: the last line is
                                           // emitted once and the player parks (paused).

  QVERIFY(!player.isPlaying());
  QVERIFY(!player.rawLivePositionMs().has_value()); // Paused: no re-anchor.
  QCOMPARE(player.currentPositionMs(), std::numeric_limits<qint64>::max());

  player.setPlaybackRate(2.0);
  QVERIFY(!player.isPlaying());
  QCOMPARE(player.playbackRate(), 2.0); // The rate change itself still applies.
  QCOMPARE(player.currentPositionMs(), std::numeric_limits<qint64>::max()); // NOT 0.

  player.setOffset(0); // The tag keeps the total at max (max + 0 + 60).
  QCOMPARE(player.offset(), std::numeric_limits<qint64>::max());
  QVERIFY(!player.isPlaying());
  QCOMPARE(player.currentPositionMs(), std::numeric_limits<qint64>::max()); // Still NOT 0.
}

void TestLyricPlayer::negativeSaturatedOffsetLiveReanchorKeepsRawPosition()
{
  // REGRESSION (raw recovery, negative saturation): raw -5000 with the
  // saturated min total offset (min + 60 line mode) anchors the inclusive
  // clock at min, and the old subtractive reconstruction returned -60
  // (min - (min + 60)) — a bogus value 4940 ms off that a live re-anchor
  // would have replayed. The raw channel must report the caller's raw
  // position while playing, and the re-anchor must keep playback safe.
  LyricPlayer player;
  player.setLyric(kHugeNegativeOffsetLrc); // Total offset: min + 60.
  QCOMPARE(player.offset(), std::numeric_limits<qint64>::min() + 60);
  player.play(-5000); // Inclusive anchor saturates at min: the player parks
                      // in the (-1,"") lead-in and KEEPS PLAYING.

  QVERIFY(player.isPlaying());

  // The raw channel reports the caller's raw position (~-5000), never the
  // old subtractive reconstruction (min - (min + 60) = -60).
  const std::optional<qint64> raw = player.rawLivePositionMs();
  QVERIFY(raw.has_value());
  QVERIFY(qAbs(*raw - (-5000)) < 100);

  // A live setPlaybackRate re-anchors the TRUE raw position: the clock
  // returns to the saturated min, the lead-in survives, playback stays safe.
  player.setPlaybackRate(2.0);
  QVERIFY(player.isPlaying());
  QCOMPARE(player.playbackRate(), 2.0);
  QVERIFY(qAbs(player.currentPositionMs() - std::numeric_limits<qint64>::min()) < 100);
}

void TestLyricPlayer::rawPositionRecoverySurvivesInclusiveSaturation()
{
  // The offset-INCLUSIVE anchor is not invertible once it saturates
  // (max - max would reconstruct 0), so the raw position must come from an
  // independent raw anchor that play() stores alongside the inclusive one.
  // The raw channel keeps the caller's position for BOTH saturation
  // directions, and keeps advancing with elapsed playback and the playback
  // rate — it never falls back to the initial play argument.
  const qint64 max = std::numeric_limits<qint64>::max();
  const qint64 min = std::numeric_limits<qint64>::min();

  LyricClock positive;
  positive.resync(1000, max);
  QCOMPARE(positive.currentPositionMs(), max);
  QCOMPARE(positive.rawPositionMs(), qint64(1000)); // Never the bogus 0.

  LyricClock negative;
  negative.resync(-5000, min); // min + (-5000) saturates to min.
  QCOMPARE(negative.currentPositionMs(), min);
  QCOMPARE(negative.rawPositionMs(), qint64(-5000)); // Never 0 either.

  // Elapsed playback advances the raw channel like the inclusive one.
  const qint64 rawBefore = negative.rawPositionMs();
  const qint64 positiveBefore = positive.rawPositionMs();
  QTest::qWait(100);
  QVERIFY(negative.rawPositionMs() > rawBefore);
  QVERIFY(positive.rawPositionMs() > positiveBefore);

  // The playback rate scales the raw channel: at 4.0 the raw position
  // advances at least 4 ms per elapsed millisecond.
  negative.setRate(4.0);
  const qint64 fastBefore = negative.rawPositionMs();
  QTest::qWait(100);
  QVERIFY(negative.rawPositionMs() > fastBefore + 1);
}

void TestLyricPlayer::hugeLineTimeDelayClampsToTimerRange()
{
  // A line timestamp beyond the QTimer int range (3.6e9 ms): the delay must
  // clamp to the supported int maximum before the narrowing conversion,
  // instead of feeding QTimer::start() an overflowed (negative) interval
  // that fires immediately or spins.
  LyricPlayer player;
  QVector<QPair<int, QString>> emissions;
  QObject::connect(&player, &LyricPlayer::lineChanged, &player,
                   [&emissions](int line, const QString& text) {
                     emissions.append({line, text});
                   });

  player.setLyric(kHugeLineTimeLrc);
  QCOMPARE(player.lines().at(0).timeMs, qint64(3596399999));
  player.play(0); // Lead-in: gap clamps to int max (~24.8 days).

  QVERIFY(player.isPlaying());
  QCOMPARE(emissions.size(), 1);
  QCOMPARE(emissions.first().first, -1);

  QTest::qWait(100); // No line emission, no 0 ms spin.
  QVERIFY(player.isPlaying());
  QCOMPARE(emissions.size(), 1);
}

void TestLyricPlayer::clockSetRateRejectsInvalidRates()
{
  // Defense in depth at the CLOCK level: a direct caller can bypass the
  // controller's protocol boundary and the player's setter, so
  // LyricClock::setRate must itself reject any rate that would poison the
  // scaled-elapsed math (NaN defeats the comparison guard and reaches
  // qRound64 — undefined behavior) or distort/reverse the position.
  LyricClock clock;
  clock.resync(1000, 0); // Anchor the inclusive and raw channels at 1000 ms.
  QCOMPARE(clock.rate(), 1.0);

  clock.setRate(2.0);
  QCOMPARE(clock.rate(), 2.0);

  // NaN, both infinities, zero, negatives, tiny and out-of-range values must
  // never replace the valid current rate — the player's exact policy.
  clock.setRate(std::numeric_limits<double>::quiet_NaN());
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(std::numeric_limits<double>::infinity());
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(-std::numeric_limits<double>::infinity());
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(0.0);
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(-0.0);
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(-1.0);
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(-0.5);
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(0.01); // Below kMinPlaybackRate (tiny rate).
  QCOMPARE(clock.rate(), 2.0);
  clock.setRate(100.0); // Above kMaxPlaybackRate.
  QCOMPARE(clock.rate(), 2.0);

  // Rejection preserves the anchored POSITION too: the retained 2.0 rate
  // scales only the tiny elapsed time since resync, far below 100 ms — a
  // poisoned NaN rate would have made the position unreadable.
  QVERIFY(qAbs(clock.currentPositionMs() - 1000) < 100);
  QVERIFY(qAbs(clock.rawPositionMs() - 1000) < 100);

  // A fresh (unanchored) clock reports its base position EXACTLY: nothing in
  // a rejected call can move even the base, and the default rate survives.
  LyricClock fresh;
  fresh.setRate(std::numeric_limits<double>::quiet_NaN());
  fresh.setRate(-std::numeric_limits<double>::infinity());
  QCOMPARE(fresh.rate(), 1.0);
  QCOMPARE(fresh.currentPositionMs(), qint64(0));
  QCOMPARE(fresh.rawPositionMs(), qint64(0));

  // Valid rates still apply; the boundary values themselves are accepted.
  clock.setRate(LyricPlayer::kMinPlaybackRate);
  QCOMPARE(clock.rate(), LyricPlayer::kMinPlaybackRate);
  clock.setRate(LyricPlayer::kMaxPlaybackRate);
  QCOMPARE(clock.rate(), LyricPlayer::kMaxPlaybackRate);
  clock.setRate(0.5);
  QCOMPARE(clock.rate(), 0.5);
}

void TestLyricPlayer::clockValidRateStillScalesElapsed()
{
  // The rejection guard must not change valid-rate behavior: a valid rate
  // still scales elapsed wall time. Wide deterministic margin — at 4.0x,
  // 50 ms of wall time means at least 200 ms of clock progress, so the
  // assertion can never flake on timer slop (same pattern as
  // rawPositionRecoverySurvivesInclusiveSaturation).
  LyricClock clock;
  clock.resync(0, 0);
  clock.setRate(4.0);

  const qint64 before = clock.currentPositionMs();
  QTest::qWait(50);
  QVERIFY(clock.currentPositionMs() > before + 50);

  // A rejected NaN must leave the clock advancing at the retained valid
  // rate — not frozen, not poisoned into a NaN position.
  clock.setRate(std::numeric_limits<double>::quiet_NaN());
  QCOMPARE(clock.rate(), 4.0);
  const qint64 afterRejection = clock.currentPositionMs();
  QTest::qWait(50);
  QVERIFY(clock.currentPositionMs() > afterRejection + 40);
}

QTEST_MAIN(TestLyricPlayer)

#include "tst_lyricplayer.moc"
