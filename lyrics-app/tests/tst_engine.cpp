/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QTest>

#include "engine/lrcparser.h"
#include "engine/lyricselector.h"
#include "engine/wordparser.h"

// Tests for the pure lyric parsers ported from lx-music-desktop:
// - LrcParser     <- references/.../line-player.js (tags, timed lines, extended lyrics)
// - WordParser    <- references/.../font-player.js _parseLyric (karaoke words)
// - LyricSelector <- references/.../worker/main/music.ts parseLyric + .../lyric.ts setLyric
//                    ([awlrc:...] container extraction, lyric/extension selection)
//
// Multi-line parse behavior is a documented, deliberate deviation from the JS
// reference: line-player.js's stateful /g exec drops every other timed line
// (see kTimeFieldExp in lrcparser.cpp), while this port parses every line.
class TestEngine : public QObject {
    Q_OBJECT

private slots:
    void lrcParserSingleLine();
    void lrcParserMultiTimeTagMergesIntoExtendedLyrics();
    void lrcParserDuplicateTimestampsMerge();
    void lrcParserOffsetTag();
    void lrcParserLinesSortedByTime();
    void lrcParserExtendedLyricsParam();
    void lrcParserSkipsExtendedLyricCommentLines();
    void lrcParserStrippedTimestampKeyMerge();
    void lrcParserFractionalSecondsMatchJs();
    void lrcParserEmptyLyric();
    void lrcParserCrlfLineEndings();
    void lrcParserNoFractionalSecondsDropped();
    void lrcParserManyColonFieldsParsedAtZero();
    void lrcParserNegativeOffset();
    void lrcParserHexOffsetDefaultsZero();

    void wordParserKaraokeWords();
    void wordParserLineModePlainText();
    void wordParserLeadingTextFlipsToLineMode();
    void wordParserPureKaraoke();
    void wordParserHasTimeTags();
    void wordParserEmptyText();
    void wordParserUnclosedTagLineMode();
    void wordParserSingleTagEmptyWord();
    void wordParserLeadingTextMultipleTagsFlipsToLineMode();

    void awlrcExtractFull();
    void awlrcIgnoresInvalidParts();
    void awlrcFirstMatchOnly();
    void selectorChoosesLxlrcWhenEnabledAndPresent();
    void selectorPrefersLrcWhenLxlrcEmpty();
    void selectorExtendedLyricsOrder();
    void selectorHasLyricsFalseWhenEmpty();
};

void TestEngine::lrcParserSingleLine()
{
    const LrcParser::Result result = LrcParser::parse(QStringLiteral("[00:01.00]Hello"));

    QCOMPARE(result.lines.size(), 1);
    QCOMPARE(result.lines.at(0).timeMs, qint64(1000));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("Hello"));
    QCOMPARE(result.lines.at(0).extendedLyrics.size(), 0);
}

void TestEngine::lrcParserMultiTimeTagMergesIntoExtendedLyrics()
{
    // Both time tags strip to the same label "0:1.0"; the second occurrence
    // pushes the text into the first line's extendedLyrics (JS linesMap merge).
    const LrcParser::Result result = LrcParser::parse(QStringLiteral("[00:01.00][00:01.00]Hello"));

    QCOMPARE(result.lines.size(), 1);
    QCOMPARE(result.lines.at(0).timeMs, qint64(1000));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("Hello"));
    QCOMPARE(result.lines.at(0).extendedLyrics, QStringList{ QStringLiteral("Hello") });
}

void TestEngine::lrcParserDuplicateTimestampsMerge()
{
    // Two lines at the same timestamp merge: the later text becomes an
    // extended lyric of the first line.
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[00:01.00]First\n[00:01.00]Second"));

    QCOMPARE(result.lines.size(), 1);
    QCOMPARE(result.lines.at(0).timeMs, qint64(1000));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("First"));
    QCOMPARE(result.lines.at(0).extendedLyrics, QStringList{ QStringLiteral("Second") });
}

void TestEngine::lrcParserOffsetTag()
{
    const LrcParser::Result withOffset = LrcParser::parse(
        QStringLiteral("[offset:1500]\n[00:01.00]Hello"));
    QCOMPARE(withOffset.tag.offsetMs, qint64(1500));

    // parseInt of a non-numeric value is NaN -> 0 (JS Number.isNaN check).
    const LrcParser::Result badOffset = LrcParser::parse(
        QStringLiteral("[offset:abc]\n[00:01.00]Hello"));
    QCOMPARE(badOffset.tag.offsetMs, qint64(0));
}

void TestEngine::lrcParserLinesSortedByTime()
{
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[00:05.00]B\n[00:01.00]A\n[00:03.00]C"));

    QCOMPARE(result.lines.size(), 3);
    QCOMPARE(result.lines.at(0).timeMs, qint64(1000));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("A"));
    QCOMPARE(result.lines.at(1).timeMs, qint64(3000));
    QCOMPARE(result.lines.at(1).text, QStringLiteral("C"));
    QCOMPARE(result.lines.at(2).timeMs, qint64(5000));
    QCOMPARE(result.lines.at(2).text, QStringLiteral("B"));
}

void TestEngine::lrcParserExtendedLyricsParam()
{
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[00:01.00]Hello\n[00:02.00]World"),
        { QStringLiteral("[00:01.00]Tr1\n[00:02.00]//\n[00:02.00]Tr2\n[00:09.00]Orphan") });

    QCOMPARE(result.lines.size(), 2);
    QCOMPARE(result.lines.at(0).text, QStringLiteral("Hello"));
    QCOMPARE(result.lines.at(0).extendedLyrics, QStringList{ QStringLiteral("Tr1") });
    QCOMPARE(result.lines.at(1).text, QStringLiteral("World"));
    // "//" is skipped; "Orphan" has no matching timed line so it is dropped.
    QCOMPARE(result.lines.at(1).extendedLyrics, QStringList{ QStringLiteral("Tr2") });
}

void TestEngine::lrcParserSkipsExtendedLyricCommentLines()
{
    // JS parseExtendedLyric: text must be non-empty and != "//".
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[00:01.00]Hello"),
        { QStringLiteral("[00:01.00]//\n[00:01.00]Real") });

    QCOMPARE(result.lines.size(), 1);
    QCOMPARE(result.lines.at(0).extendedLyrics, QStringList{ QStringLiteral("Real") });
}

void TestEngine::lrcParserStrippedTimestampKeyMerge()
{
    // formatTimeLabel is the map key, so "[00:01.00]" and "[00:01.0]" both key
    // to "0:1.0" and merge. (Under the exact JS t_rxp_3 the pair ".50"/".5"
    // does NOT merge because ".50" has no leading zero after the dot — see
    // lrcParserFractionalSecondsMatchJs.)
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[00:01.00]A\n[00:01.0]B"));

    QCOMPARE(result.lines.size(), 1);
    QCOMPARE(result.lines.at(0).timeMs, qint64(1000));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("A"));
    QCOMPARE(result.lines.at(0).extendedLyrics, QStringList{ QStringLiteral("B") });
}

void TestEngine::lrcParserFractionalSecondsMatchJs()
{
    // Exact JS t_rxp_3 (/\.0+(\d+)/) behaviour: ".50" is left as-is, so
    // "[00:01.50]" keys "0:1.50" (1050 ms) and "[00:01.5]" keys "0:1.5"
    // (1005 ms) — two distinct lines, sorted by time.
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[00:01.50]A\n[00:01.5]B"));

    QCOMPARE(result.lines.size(), 2);
    QCOMPARE(result.lines.at(0).timeMs, qint64(1005));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("B"));
    QCOMPARE(result.lines.at(1).timeMs, qint64(1050));
    QCOMPARE(result.lines.at(1).text, QStringLiteral("A"));
}

void TestEngine::lrcParserEmptyLyric()
{
    const LrcParser::Result result = LrcParser::parse(QStringLiteral(""));

    QCOMPARE(result.lines.size(), 0);
    QCOMPARE(result.tag.title, QStringLiteral(""));
    QCOMPARE(result.tag.artist, QStringLiteral(""));
    QCOMPARE(result.tag.album, QStringLiteral(""));
    QCOMPARE(result.tag.by, QStringLiteral(""));
    QCOMPARE(result.tag.offsetMs, qint64(0));
}

void TestEngine::lrcParserCrlfLineEndings()
{
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[00:01.00]A\r\n[00:02.00]B"));

    QCOMPARE(result.lines.size(), 2);
    QCOMPARE(result.lines.at(0).timeMs, qint64(1000));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("A"));
    QCOMPARE(result.lines.at(1).timeMs, qint64(2000));
    QCOMPARE(result.lines.at(1).text, QStringLiteral("B"));
}

void TestEngine::lrcParserNoFractionalSecondsDropped()
{
    // kTimeExp requires the (?:\.\d{1,3}) group, so "[00:01]" produces no match
    // and the line is silently dropped — faithful to JS (times == null).
    const LrcParser::Result result = LrcParser::parse(QStringLiteral("[00:01]Hello"));

    QCOMPARE(result.lines.size(), 0);
}

void TestEngine::lrcParserManyColonFieldsParsedAtZero()
{
    // The JS "> 3 colon-separated fields" guard is unreachable: kTimeExp matches
    // at most "hh:mm:ss.mmm" (three colon fields), so "[00:00:00:00.00]X" still
    // parses — its first three fields are zero, giving a single line at 0 ms.
    const LrcParser::Result result = LrcParser::parse(QStringLiteral("[00:00:00:00.00]X"));

    QCOMPARE(result.lines.size(), 1);
    QCOMPARE(result.lines.at(0).timeMs, qint64(0));
    QCOMPARE(result.lines.at(0).text, QStringLiteral("X"));
}

void TestEngine::lrcParserNegativeOffset()
{
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[offset:-500]\n[00:01.00]Hello"));

    QCOMPARE(result.tag.offsetMs, qint64(-500));
}

void TestEngine::lrcParserHexOffsetDefaultsZero()
{
    // toInt() uses base 10, so "0x10" is not a number and the tag defaults to 0.
    // Deliberate deviation: JS parseInt("0x10") would return 16.
    const LrcParser::Result result = LrcParser::parse(
        QStringLiteral("[offset:0x10]\n[00:01.00]Hello"));

    QCOMPARE(result.tag.offsetMs, qint64(0));
}

void TestEngine::wordParserKaraokeWords()
{
    const WordParser::Result result = WordParser::parse(
        QStringLiteral("<1000,500>你<1600,300>好"));

    QVERIFY(!result.isLineMode);
    QCOMPARE(result.words.size(), 2);
    QCOMPARE(result.words.at(0).text, QStringLiteral("你"));
    QCOMPARE(result.words.at(0).startMs, qint64(1000));
    QCOMPARE(result.words.at(0).durationMs, qint64(500));
    QCOMPARE(result.words.at(1).text, QStringLiteral("好"));
    QCOMPARE(result.words.at(1).startMs, qint64(1600));
    QCOMPARE(result.words.at(1).durationMs, qint64(300));
}

void TestEngine::wordParserLineModePlainText()
{
    const WordParser::Result result = WordParser::parse(QStringLiteral("plain lyric text"));

    QVERIFY(result.isLineMode);
    QCOMPARE(result.words.size(), 1);
    QCOMPARE(result.words.at(0).text, QStringLiteral("plain lyric text"));
    QCOMPARE(result.words.at(0).startMs, qint64(0));
    QCOMPARE(result.words.at(0).durationMs, qint64(0));
}

void TestEngine::wordParserLeadingTextFlipsToLineMode()
{
    // JS bug-compatible: the first split segment "ABC" has no time tag, so the
    // whole line falls back to line mode even though later segments do.
    const WordParser::Result result = WordParser::parse(QStringLiteral("ABC<1000,500>X"));

    QVERIFY(result.isLineMode);
    QCOMPARE(result.words.size(), 1);
    QCOMPARE(result.words.at(0).text, QStringLiteral("ABC<1000,500>X"));
}

void TestEngine::wordParserPureKaraoke()
{
    // Line starts with a tag: no leading segment, both segments carry tags.
    const WordParser::Result result = WordParser::parse(
        QStringLiteral("<1000,500>X<1600,300>Y"));

    QVERIFY(!result.isLineMode);
    QCOMPARE(result.words.size(), 2);
    QCOMPARE(result.words.at(0).text, QStringLiteral("X"));
    QCOMPARE(result.words.at(0).startMs, qint64(1000));
    QCOMPARE(result.words.at(0).durationMs, qint64(500));
    QCOMPARE(result.words.at(1).text, QStringLiteral("Y"));
    QCOMPARE(result.words.at(1).startMs, qint64(1600));
    QCOMPARE(result.words.at(1).durationMs, qint64(300));
}

void TestEngine::wordParserHasTimeTags()
{
    QVERIFY(WordParser::hasTimeTags(QStringLiteral("<1000,500>X")));
    QVERIFY(!WordParser::hasTimeTags(QStringLiteral("ABC<1000,500>X")));
    QVERIFY(!WordParser::hasTimeTags(QStringLiteral("")));
}

void TestEngine::wordParserEmptyText()
{
    const WordParser::Result result = WordParser::parse(QStringLiteral(""));

    QVERIFY(result.isLineMode);
    QCOMPARE(result.words.size(), 1);
    QCOMPARE(result.words.at(0).text, QStringLiteral(""));
    QCOMPARE(result.words.at(0).startMs, qint64(0));
    QCOMPARE(result.words.at(0).durationMs, qint64(0));
}

void TestEngine::wordParserUnclosedTagLineMode()
{
    // "<1000" has no <\d+,\d+> tag, so the whole line falls back to line mode.
    const WordParser::Result result = WordParser::parse(QStringLiteral("<1000"));

    QVERIFY(result.isLineMode);
    QCOMPARE(result.words.size(), 1);
    QCOMPARE(result.words.at(0).text, QStringLiteral("<1000"));
}

void TestEngine::wordParserSingleTagEmptyWord()
{
    const WordParser::Result result = WordParser::parse(QStringLiteral("<1,2>"));

    QVERIFY(!result.isLineMode);
    QCOMPARE(result.words.size(), 1);
    QCOMPARE(result.words.at(0).text, QStringLiteral(""));
    QCOMPARE(result.words.at(0).startMs, qint64(1));
    QCOMPARE(result.words.at(0).durationMs, qint64(2));
}

void TestEngine::wordParserLeadingTextMultipleTagsFlipsToLineMode()
{
    // JS bug-compatible: the leading segment "A" (before the first tag) has no
    // time tag, so the whole line falls back to line mode even though the later
    // segments "B" and "C" both carry tags.
    const WordParser::Result result = WordParser::parse(
        QStringLiteral("A<1000,500>B<2000,300>C"));

    QVERIFY(result.isLineMode);
    QCOMPARE(result.words.size(), 1);
    QCOMPARE(result.words.at(0).text, QStringLiteral("A<1000,500>B<2000,300>C"));
}

void TestEngine::awlrcExtractFull()
{
    // Base64 (computed, not from the draft): "WzAwOjAxLjAwXVNvbmc=" is
    // "[00:01.00]Song" and "WzAwOjAwLjAwXTwwLDk5OTk+SGVsbG8=" is
    // "[00:00.00]<0,9999>Hello". The awlrc part must carry a "[time]" prefix to
    // pass the awlrc verify; the bare "<0,9999>Hello" form is rejected by the JS
    // verifyAwlrc and covered in awlrcIgnoresInvalidParts.
    const QString lrc = QStringLiteral(
        "intro\n[awlrc:lrc:WzAwOjAxLjAwXVNvbmc=,awlrc:WzAwOjAwLjAwXTwwLDk5OTk+SGVsbG8=]\n[00:01.00]Song");

    QString stripped;
    const AwlrcParts parts = LyricSelector::extractAwlrc(lrc, &stripped);

    QCOMPARE(stripped, QStringLiteral("intro\n[00:01.00]Song"));
    QCOMPARE(parts.lyric, QStringLiteral("[00:01.00]Song"));
    QCOMPARE(parts.lxlyric, QStringLiteral("[00:00.00]<0,9999>Hello"));
    QVERIFY(parts.found);
}

void TestEngine::awlrcIgnoresInvalidParts()
{
    // "tlrc:aaaa" decodes to garbage bytes (0x69 0xA6 0x9A) that fail the lrc
    // verify, and "awlrc:PDAsOTk5OT5IZWxsbw==" decodes to the bare
    // "<0,9999>Hello" which lacks the "[time]" prefix the awlrc verify
    // requires. Both parts are silently dropped; the valid lrc part is kept.
    const QString lrc = QStringLiteral(
        "[awlrc:lrc:WzAwOjAxLjAwXVNvbmc=,tlrc:aaaa,awlrc:PDAsOTk5OT5IZWxsbw==]");

    QString stripped;
    const AwlrcParts parts = LyricSelector::extractAwlrc(lrc, &stripped);

    QCOMPARE(parts.lyric, QStringLiteral("[00:01.00]Song"));
    QCOMPARE(parts.tlyric, QStringLiteral(""));
    QCOMPARE(parts.lxlyric, QStringLiteral(""));
    QVERIFY(parts.found);
}

void TestEngine::awlrcFirstMatchOnly()
{
    // JS tagRxp has no /g flag, so only the FIRST [awlrc:...] tag is parsed and
    // removed; the second tag stays in the stripped text.
    const QString lrc = QStringLiteral(
        "[awlrc:lrc:WzAwOjAxLjAwXVNvbmc=]one\n[awlrc:lrc:WzAwOjAyLjAwXVNvbmc=]two");

    QString stripped;
    const AwlrcParts parts = LyricSelector::extractAwlrc(lrc, &stripped);

    QCOMPARE(parts.lyric, QStringLiteral("[00:01.00]Song"));
    QCOMPARE(stripped, QStringLiteral("one\n[awlrc:lrc:WzAwOjAyLjAwXVNvbmc=]two"));
    QVERIFY(parts.found);
}

void TestEngine::selectorChoosesLxlrcWhenEnabledAndPresent()
{
    // Defaults match lx-music defaultSetting: isPlayLxlrc is true on Linux.
    LyricSelector selector;
    selector.setLyrics(QStringLiteral("L"), QString(), QString(), QStringLiteral("W"));

    QCOMPARE(selector.selectedLyric(), QStringLiteral("W"));
}

void TestEngine::selectorPrefersLrcWhenLxlrcEmpty()
{
    LyricSelector selector;
    selector.setLyrics(QStringLiteral("L"), QString(), QString(), QString());

    QCOMPARE(selector.selectedLyric(), QStringLiteral("L"));
}

void TestEngine::selectorExtendedLyricsOrder()
{
    // JS setLyric pushes rlyric before tlyric, then reverses when swapped.
    LyricSelector selector;
    selector.setIsShowLyricTranslation(true);
    selector.setIsShowLyricRoma(true);
    selector.setLyrics(QStringLiteral("L"), QStringLiteral("T"), QStringLiteral("R"), QString());

    QCOMPARE(selector.extendedLyrics(), QStringList({ QStringLiteral("R"), QStringLiteral("T") }));

    selector.setIsSwapLyricTranslationAndRoma(true);
    QCOMPARE(selector.extendedLyrics(), QStringList({ QStringLiteral("T"), QStringLiteral("R") }));
}

void TestEngine::selectorHasLyricsFalseWhenEmpty()
{
    LyricSelector selector;

    QCOMPARE(selector.selectedLyric(), QStringLiteral(""));
    QVERIFY(!selector.hasLyrics());
}

QTEST_MAIN(TestEngine)

#include "tst_engine.moc"
