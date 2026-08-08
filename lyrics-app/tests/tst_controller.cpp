/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
// Integration tests for the lyric pipeline controller. Invalid timestamps
// ("[00:00.-1]..." from Skyland - Mich.lrc) now parse to STATIC lyric lines —
// rendered but never visited — and an all-static lyric is shown as a custom
// placeholder: the static text itself, centered like the no-lyrics
// placeholder, never active. The empty-raw-text path pins the pre-existing
// metadata placeholder behavior, and a valid lyric proves real lines still
// bypass both placeholders. stop() clears the player, whose zero-line push
// must become the metadata placeholder, and toggling the metadata key while
// a zero-line lyric is current (empty or tag-only) must re-apply the
// placeholder live (it must NOT affect static lyrics).
//
// The user's real ~/.config/lx-lyrics/config.json is never touched:
// DesktopLyricConfig::~DesktopLyricConfig() flushes unconditionally, so the
// static init block below redirects the config dir to a per-process temp dir
// (same recipe as tst_config.cpp initTestCase).
#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTest>

#include "app/appcontext.h"
#include "app/clioptions.h"
#include "app/lyriccontroller.h"
#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"
#include "renderer/lyricrenderer.h"
#include "window/lyricwindow.h"

// Run headless: the window needs no real display server, and the suite must
// work in CI/ctest without one. (Static init runs before QApplication.)
static const bool s_forceOffscreen = [] {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}();

// Config isolation (never touch the developer's real tuned config): test mode
// redirects QStandardPaths::ConfigLocation under the temp dir, and
// XDG_CONFIG_HOME is overridden as a second safety net — exactly the recipe
// tst_config.cpp uses in initTestCase. Every DesktopLyricConfig in this suite
// flushes on destruction, so without this the tests would clobber
// ~/.config/lx-lyrics/config.json with defaults. (setTestModeEnabled affects
// QStandardPaths globally; that is the accepted pattern, see tst_config.)
static const bool s_configIsolation = [] {
    QStandardPaths::setTestModeEnabled(true);
    qputenv("XDG_CONFIG_HOME",
            (QDir::tempPath() + QStringLiteral("/lxlyrics-controller-test-")
             + QString::number(QCoreApplication::applicationPid())).toUtf8());
    return true;
}();

class TestLyricController : public QObject {
    Q_OBJECT

private slots:
    void invalidTimestampsRenderAsStaticLines();
    void emptyLyricShowsPlaceholder();
    void validLyricShowsParsedLines();
    void stopAfterValidLyricShowsPlaceholder();
    void metadataToggleReappliesPlaceholder();
    void mixedLyricNeverVisitsStaticLine();
};

void TestLyricController::invalidTimestampsRenderAsStaticLines()
{
    // Skyland - Mich.lrc regression: the '-' in "[00:00.-1]" is not in
    // kTimeFieldExp's [\d:.] class, so the text after it becomes a STATIC
    // lyric line (rendered, never visited). An all-static lyric is shown as a
    // CUSTOM placeholder: the static text itself, centered like the no-lyrics
    // placeholder, never active/colored. DELIBERATE DEVIATION from
    // line-player.js, which drops such lines (they used to fall through to
    // the metadata placeholder here).
    AppContext ctx(CliOptions{});
    ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
    TranslationManager i18n(ctx.config);
    LyricWindow window(ctx.config, i18n);
    LyricController controller(ctx, window);

    TrackSnapshot snapshot;
    snapshot.name = QStringLiteral("Skyland - Mich");
    snapshot.singer = QStringLiteral("Michal Wisniewski");
    snapshot.album = QStringLiteral("core");
    snapshot.lrc = QStringLiteral(
        "[00:00.-1]作词: Michal Wisniewski\n[00:00.-1]作曲: Michal Wisniewski");
    controller.setTrack(snapshot);

    auto* renderer = window.contentContainer()->findChild<LyricRenderer*>();
    QVERIFY(renderer != nullptr);
    // The static text itself — NOT the 3-line metadata placeholder.
    QCOMPARE(renderer->lineCount(), 2);
    // Custom placeholder presentation: centered block, like the no-lyrics
    // placeholder.
    QVERIFY(renderer->centeredBlock());
    // Static lines are never visited: no played color, ever.
    QCOMPARE(renderer->colorProgressForLine(0), 0.0);
    QCOMPARE(renderer->colorProgressForLine(1), 0.0);
}

void TestLyricController::emptyLyricShowsPlaceholder()
{
    // The pre-existing no-lyrics path: an empty raw lyric shows the track
    // placeholder (name/singer/album) instead of a blank pane.
    AppContext ctx(CliOptions{});
    ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
    TranslationManager i18n(ctx.config);
    LyricWindow window(ctx.config, i18n);
    LyricController controller(ctx, window);

    TrackSnapshot snapshot;
    snapshot.name = QStringLiteral("Skyland - Mich");
    snapshot.singer = QStringLiteral("Michal Wisniewski");
    snapshot.album = QStringLiteral("core");
    // lrc stays empty: no lyric at all.
    controller.setTrack(snapshot);

    auto* renderer = window.contentContainer()->findChild<LyricRenderer*>();
    QVERIFY(renderer != nullptr);
    QCOMPARE(renderer->lineCount(), 3);
}

void TestLyricController::validLyricShowsParsedLines()
{
    // Real lyrics still bypass the placeholder: the two parsed timed lines
    // are pushed to the renderer as-is.
    AppContext ctx(CliOptions{});
    ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
    TranslationManager i18n(ctx.config);
    LyricWindow window(ctx.config, i18n);
    LyricController controller(ctx, window);

    TrackSnapshot snapshot;
    snapshot.name = QStringLiteral("Skyland - Mich");
    snapshot.singer = QStringLiteral("Michal Wisniewski");
    snapshot.album = QStringLiteral("core");
    snapshot.lrc = QStringLiteral("[00:01.00]Hello\n[00:02.00]World");
    controller.setTrack(snapshot);

    auto* renderer = window.contentContainer()->findChild<LyricRenderer*>();
    QVERIFY(renderer != nullptr);
    QCOMPARE(renderer->lineCount(), 2);
}

void TestLyricController::stopAfterValidLyricShowsPlaceholder()
{
    // stop() clears the player's lines; the resulting zero-line
    // lyricsChanged push must show the no-lyrics placeholder (name/singer/
    // album), never a blank pane.
    AppContext ctx(CliOptions{});
    ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
    TranslationManager i18n(ctx.config);
    LyricWindow window(ctx.config, i18n);
    LyricController controller(ctx, window);

    TrackSnapshot snapshot;
    snapshot.name = QStringLiteral("Skyland - Mich");
    snapshot.singer = QStringLiteral("Michal Wisniewski");
    snapshot.album = QStringLiteral("core");
    snapshot.lrc = QStringLiteral("[00:01.00]Hello\n[00:02.00]World");
    controller.setTrack(snapshot);

    auto* renderer = window.contentContainer()->findChild<LyricRenderer*>();
    QVERIFY(renderer != nullptr);
    QCOMPARE(renderer->lineCount(), 2);

    controller.stop();
    // The placeholder (name/singer/album) replaces the cleared lyric lines.
    QCOMPARE(renderer->lineCount(), 3);
}

void TestLyricController::metadataToggleReappliesPlaceholder()
{
    // Toggling desktopLyric.isShowNoLyricMetadata while a TAG-ONLY lyric
    // ([ti:Title]) is current must re-apply the placeholder live: the lyric
    // parses to ZERO lines while its raw text is non-empty, so the toggle
    // drives reapplyLyricSelection, whose setLyric dedups (no lyricsChanged)
    // and whose lines().isEmpty() guard re-shows the placeholder with the new
    // metadata setting. The tag-only fixture discriminates the current
    // lines().isEmpty() guard from the old hasLyrics() guard (which would
    // skip the re-apply and leave the placeholder unchanged). The toggle must
    // NOT affect static lyrics (they are real text, not the metadata
    // placeholder), so the invalid-timestamp fixture no longer applies here.
    AppContext ctx(CliOptions{});
    ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
    TranslationManager i18n(ctx.config);
    LyricWindow window(ctx.config, i18n);
    LyricController controller(ctx, window);

    TrackSnapshot snapshot;
    snapshot.name = QStringLiteral("Skyland - Mich");
    snapshot.singer = QStringLiteral("Michal Wisniewski");
    snapshot.album = QStringLiteral("core");
    // lrc = "[ti:Title]": a tag-only lyric that parses to zero lines while
    // the raw text is non-empty, so the metadata placeholder shows.
    snapshot.lrc = QStringLiteral("[ti:Title]");
    controller.setTrack(snapshot);

    auto* renderer = window.contentContainer()->findChild<LyricRenderer*>();
    QVERIFY(renderer != nullptr);
    QCOMPARE(renderer->lineCount(), 3);

    // The public config setter emits settingChanged synchronously, which is
    // the exact production path the controller's private onSettingChanged slot
    // is wired to — no meta-object invocation needed.
    QVERIFY(ctx.config.set(QStringLiteral("desktopLyric.isShowNoLyricMetadata"), false));

    // Metadata off: the placeholder collapses to the plain "No lyrics" line.
    QCOMPARE(renderer->lineCount(), 1);
}

void TestLyricController::mixedLyricNeverVisitsStaticLine()
{
    // A static lead line precedes the timed lines: it must never become the
    // active line or take the played color, and a mixed lyric must not flip
    // to the centered placeholder. All assertions run on state reached
    // synchronously by play() — no event-loop spinning needed.
    AppContext ctx(CliOptions{});
    ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
    TranslationManager i18n(ctx.config);
    LyricWindow window(ctx.config, i18n);
    LyricController controller(ctx, window);

    TrackSnapshot snapshot;
    snapshot.name = QStringLiteral("Skyland - Mich");
    snapshot.singer = QStringLiteral("Michal Wisniewski");
    snapshot.album = QStringLiteral("core");
    snapshot.lrc = QStringLiteral(
        "[00:00.-1]作词: X\n[00:01.00]Hello\n[00:02.00]World");
    controller.setTrack(snapshot);

    auto* renderer = window.contentContainer()->findChild<LyricRenderer*>();
    QVERIFY(renderer != nullptr);
    QCOMPARE(renderer->lineCount(), 3);
    QVERIFY(!renderer->centeredBlock()); // Mixed lyric: normal scroll layout.

    // Pre-play the player parks on the static lead line (index 0) and reports
    // -1, so the renderer must have no active line — a static line would be
    // active as 0 only if BOTH the currentLine() guard and the renderer clamp
    // were reverted.
    QCOMPARE(renderer->activeLine(), -1);

    controller.play(1500); // Line 1 ("Hello", 1000ms) becomes active.

    // lineChanged -> setActiveLine is direct/synchronous, so the timed
    // "Hello" line is active right away.
    QCOMPARE(renderer->activeLine(), 1);

    // The static line 0 precedes the active line 1 and must stay unplayed —
    // never colored, even though play() advanced past its (nonexistent) time.
    QCOMPARE(renderer->colorProgressForLine(0), 0.0);
}

QTEST_MAIN(TestLyricController)
#include "tst_controller.moc"
