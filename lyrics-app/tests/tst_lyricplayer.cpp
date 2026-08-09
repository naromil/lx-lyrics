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

QTEST_MAIN(TestLyricPlayer)

#include "tst_lyricplayer.moc"
