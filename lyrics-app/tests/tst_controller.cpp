/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
// Integration tests for the lyric pipeline controller. Invalid timestamps
// ("[00:00.-1]..." from Skyland - Mich.lrc) now parse to STATIC lyric lines —
// rendered but never visited — and an all-static lyric is shown as a custom
// placeholder: the static text itself, centered like the no-lyrics
// placeholder, never active. Bare unsynced text (no timestamps at all) takes
// the same path. The empty-raw-text path pins the pre-existing metadata
// placeholder behavior, and a valid lyric proves real lines still bypass
// both placeholders. stop() clears the player, whose zero-line push must
// become the metadata placeholder, and toggling the metadata key while a
// zero-line lyric is current (empty or tag-only) must re-apply the
// placeholder live (it must NOT affect static lyrics).
//
// The user's real ~/.config/lx-lyrics/config.json is never touched:
// DesktopLyricConfig::~DesktopLyricConfig() flushes unconditionally, so the
// static init block below redirects the config dir to a per-process temp dir
// (same recipe as tst_config.cpp initTestCase).
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>

#include "app/appcontext.h"
#include "app/clioptions.h"
#include "app/lyriccontroller.h"
#include "bridge/pausehide.h"
#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"
#include "renderer/controlbar.h"
#include "renderer/lyricrenderer.h"
#include "window/lyricwindow.h"

// Config isolation (never touch the developer's real tuned config): test mode
// redirects QStandardPaths::ConfigLocation under the temp dir, and
// XDG_CONFIG_HOME is overridden as a second safety net — exactly the recipe
// tst_config.cpp uses in initTestCase. Every DesktopLyricConfig in this suite
// flushes on destruction, so without this the tests would clobber
// ~/.config/lx-lyrics/config.json with defaults. (setTestModeEnabled affects
// QStandardPaths globally; that is the accepted pattern, see tst_config.)
static const bool s_configIsolation = [] {
  QStandardPaths::setTestModeEnabled(true);
  qputenv("XDG_CONFIG_HOME", (QDir::tempPath() + QStringLiteral("/lxlyrics-controller-test-") +
                              QString::number(QCoreApplication::applicationPid()))
                               .toUtf8());
  return true;
}();

class TestLyricWindow : public LyricWindow {
  Q_OBJECT
public:
  using LyricWindow::enterEvent; // Expose for choke-point testing.
  using LyricWindow::LyricWindow;
  using LyricWindow::pollHoverHide; // Expose for hover-poll testing.
};

class TestLyricController : public QObject {
  Q_OBJECT

private slots:
  void invalidTimestampsRenderAsStaticLines();
  void unsyncedLyricShowsStaticTextPlaceholder();
  void emptyLyricShowsPlaceholder();
  void validLyricShowsParsedLines();
  void stopAfterValidLyricShowsPlaceholder();
  void metadataToggleReappliesPlaceholder();
  void mixedLyricNeverVisitsStaticLine();
  void closeButtonRequestsClose();
  void animateCloseFadesThenSignals();
  void normalFaintDoesNotEmitCloseFinished();
  void closeDuringFaintStillFadesOut();
  void chokePointBlocksHoverFadeUpDuringClose();
  void animateCloseReentryIsNoOp();
  void pauseFaintSurvivesLockToggle();
  void pauseFaintSurvivesShowCycle();
  void pauseFaintSurvivesHoverPollWhileLocked();
  void pauseHideAutoFaintsAtStartup();
  void pauseHideStartupFaintCancelledByPlay();
  void pauseHideEnableMidSessionFaintsWhenPaused();
  void pauseHideEnableMidSessionStaysBrightWhenPlaying();
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
  snapshot.lrc =
    QStringLiteral("[00:00.-1]作词: Michal Wisniewski\n[00:00.-1]作曲: Michal Wisniewski");
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

void TestLyricController::unsyncedLyricShowsStaticTextPlaceholder()
{
  // Regression: a fully timestamp-less lyric (UNSYNCEDLYRICS tags, text-only
  // .lrc sidecars) used to parse to ZERO lines and show "No lyrics" / the
  // metadata placeholder. Bare text now parses to STATIC lines, so the
  // unsynced text itself shows as the custom centered placeholder — like the
  // broken-timestamp case, rendered but never active/colored.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  TrackSnapshot snapshot;
  snapshot.name = QStringLiteral("Skyland - Mich");
  snapshot.singer = QStringLiteral("Michal Wisniewski");
  snapshot.album = QStringLiteral("core");
  snapshot.lrc = QStringLiteral("作词: Michal Wisniewski\n作曲: Michal Wisniewski");
  controller.setTrack(snapshot);

  auto* renderer = window.contentContainer()->findChild<LyricRenderer*>();
  QVERIFY(renderer != nullptr);
  // The static text itself — NOT the 3-line metadata placeholder (this is
  // the regression: previously the zero-line parse showed the placeholder).
  QCOMPARE(renderer->lineCount(), 2);
  // Custom placeholder presentation: centered block, like the no-lyrics
  // placeholder.
  QVERIFY(renderer->centeredBlock());
  // Static lines are never visited: no played color, ever, and no active
  // line index is ever reported.
  QCOMPARE(renderer->colorProgressForLine(0), 0.0);
  QCOMPARE(renderer->colorProgressForLine(1), 0.0);
  QCOMPARE(renderer->activeLine(), -1);
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
  snapshot.lrc = QStringLiteral("[00:00.-1]作词: X\n[00:01.00]Hello\n[00:02.00]World");
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

void TestLyricController::closeButtonRequestsClose()
{
  // The X button no longer quits directly: it emits closeRequested, and
  // LyricWindow animates the fade-out. The window's ctor connect starts the
  // 300 ms close fade here; the test ends before it completes and no quit
  // connection exists in the test binary, so teardown just stops the fade.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  // A visible window is required for QTest::mouseClick to deliver events
  // (the offscreen test platform handles show() fine).
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));

  auto* bar = window.contentContainer()->findChild<ControlBar*>();
  QVERIFY(bar != nullptr);
  auto* closeButton = bar->findChild<QToolButton*>(QStringLiteral("closeButton"));
  QVERIFY(closeButton != nullptr);

  QSignalSpy spy(bar, &ControlBar::closeRequested);
  QTest::mouseClick(closeButton, Qt::LeftButton);
  QCOMPARE(spy.count(), 1);

  // The click's real side effect: the window's ctor wiring
  // (closeRequested -> animateClose) starts the 300 ms close fade. Safe to
  // assert here because the quit wiring lives only in main.cpp, never in the
  // test binary.
  QTRY_VERIFY(window.fadeFactor() < 0.5);
}

void TestLyricController::animateCloseFadesThenSignals()
{
  // animateClose drives the usual 300 ms content fade to 0.0 and then emits
  // closeAnimationFinished (main.cpp quits the app on it; no such connection
  // exists here, so the test only watches the signal).
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  QSignalSpy spy(&window, &LyricWindow::closeAnimationFinished);
  window.animateClose();

  QElapsedTimer timer;
  timer.start();
  while (spy.count() == 0 && timer.elapsed() < 1000)
    QTest::qWait(25);

  QCOMPARE(spy.count(), 1);
  QVERIFY(window.fadeFactor() < 0.02);
}

void TestLyricController::normalFaintDoesNotEmitCloseFinished()
{
  // NEGATIVE regression: pause-faint (faint/unfaint) completes both fade
  // directions without ever emitting closeAnimationFinished. Guards against
  // moving the finished-connection into the ctor or losing the m_closing
  // gate — the fade cycle must stay a pure content fade with zero close
  // signals.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  QSignalSpy spy(&window, &LyricWindow::closeAnimationFinished);

  window.faint();
  QTRY_VERIFY(window.fadeFactor() < 0.1);

  window.unfaint();
  QTRY_VERIFY(window.fadeFactor() > 0.99);

  QCOMPARE(spy.count(), 0);
}

void TestLyricController::closeDuringFaintStillFadesOut()
{
  // The enter/leave retarget class: a fade-up attempt while the close
  // animation runs must be ignored (enterEvent/leaveEvent hit the same
  // animateFadeTo choke point, only reachable through unfaint() here), so
  // the close still completes exactly once and the window stays faded out.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  window.faint();
  QTRY_VERIFY(window.fadeFactor() < 0.1);

  QSignalSpy spy(&window, &LyricWindow::closeAnimationFinished);
  window.animateClose();
  window.unfaint(); // Fade-up attempt during close: must be blocked.

  QElapsedTimer timer;
  timer.start();
  while (spy.count() == 0 && timer.elapsed() < 1000)
    QTest::qWait(25);

  QCOMPARE(spy.count(), 1);
  QVERIFY(window.fadeFactor() < 0.02);
}

void TestLyricController::chokePointBlocksHoverFadeUpDuringClose()
{
  // REAL choke-point regression: enterEvent/leaveEvent call animateFadeTo
  // directly (bypassing faint/unfaint), so only the m_closing guard inside
  // animateFadeTo can stop a hover retarget from cancelling the close fade
  // (which would quit the app with the window fully visible). The synthetic
  // QEnterEvent drives the exact production hover path through the exposed
  // protected handler.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
  TranslationManager i18n(ctx.config);
  TestLyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  // faint() sets m_shouldBeFaint, so a hover enter would normally brighten
  // the window back to full opacity.
  window.faint();
  QTRY_VERIFY(window.fadeFactor() < 0.1);

  QSignalSpy spy(&window, &LyricWindow::closeAnimationFinished);
  window.animateClose();

  // Hover retarget during the close fade: without the choke point this would
  // animate back to 1.0 and the app would quit fully visible.
  QEnterEvent enter(QPointF(0, 0), QPointF(0, 0), QPointF(0, 0));
  window.enterEvent(&enter);
  QTest::qWait(50);

  // Still fading down, not retargeted upward.
  QVERIFY(window.fadeFactor() < 0.1);
  QTRY_COMPARE(spy.count(), 1);
}

void TestLyricController::animateCloseReentryIsNoOp()
{
  // Idempotence: a second animateClose() during the running close fade
  // early-returns on m_closing, so exactly one closeAnimationFinished fires.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // Idempotent; the ctor already loaded them.
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  QSignalSpy spy(&window, &LyricWindow::closeAnimationFinished);
  window.animateClose();
  window.animateClose();

  QElapsedTimer timer;
  timer.start();
  while (spy.count() == 0 && timer.elapsed() < 1000)
    QTest::qWait(25);

  QCOMPARE(spy.count(), 1);
}

void TestLyricController::pauseFaintSurvivesLockToggle()
{
  // Regression (restart-brightening): the pause-faint must survive a lock
  // toggle. updateHoverHidePolling() runs on desktopLyric.isLock changes;
  // before the fix it unconditionally unfainted, so a paused window went
  // bright until the next play-state message (which never comes while
  // paused).
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults();
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  window.faint();
  QTRY_VERIFY(window.fadeFactor() < 0.1);

  ctx.config.set(QStringLiteral("desktopLyric.isLock"), true); // -> updateHoverHidePolling
  QTest::qWait(400);                  // Let any (buggy) unfade run to completion.
  QVERIFY(window.fadeFactor() < 0.1); // Still dimmed: the pause-faint survives.

  ctx.config.set(QStringLiteral("desktopLyric.isLock"), false);
  QTest::qWait(400);
  QVERIFY(window.fadeFactor() < 0.1); // Still dimmed after the toggle round-trip.

  window.unfaint(); // Sanity: the machinery still restores full brightness.
  QTRY_VERIFY(window.fadeFactor() > 0.99);
}

void TestLyricController::pauseFaintSurvivesShowCycle()
{
  // The reported bug: paused dimmed lyrics become bright again after a
  // display restart (re-show). showEvent -> updateHoverHidePolling used to
  // unfaint unconditionally; the pause-faint must survive the re-show.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults();
  TranslationManager i18n(ctx.config);
  LyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  // Lock the window so faint()'s hover rule (!isLock && underMouse()) cannot
  // brighten it when the real desktop cursor happens to be over the default
  // geometry — on Wayland the compositor ignores move(), so geometry-parking
  // cannot make this deterministic. The showEvent -> updateHoverHidePolling
  // path under test is unaffected (hoverHide is off -> inactive branch
  // regardless of lock).
  ctx.config.set(QStringLiteral("desktopLyric.isLock"), true);

  window.show(); // showEvent -> updateHoverHidePolling (hover-hide off by default).
  window.faint();
  QTRY_VERIFY(window.fadeFactor() < 0.1);

  window.hide();
  window.show(); // The display restart: showEvent fires updateHoverHidePolling again.
  QTest::qWait(400);
  QVERIFY(window.fadeFactor() < 0.1); // Still dimmed.
}

void TestLyricController::pauseFaintSurvivesHoverPollWhileLocked()
{
  // Hover-hide poll path: while hover-hide is active (locked) the 500 ms
  // cursor poll runs. A cursor-outside tick must clear only the hover source;
  // a live pause-faint must keep the window dimmed (reference
  // hide: isHide || isHoverHide).
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults();
  TranslationManager i18n(ctx.config);
  TestLyricWindow window(ctx.config, i18n);
  LyricController controller(ctx, window);

  ctx.config.set(QStringLiteral("desktopLyric.isLock"), true);
  ctx.config.set(QStringLiteral("desktopLyric.isHoverHide"), true); // Arms the 500 ms poll.

  // pollHoverHide self-stops while hidden, so the window must be shown for
  // the cursor check to run at all. Park it far away first: on the offscreen
  // test platform the cursor stays at (0,0), which can never fall inside that
  // geometry, so the poll deterministically takes the cursor-outside branch
  // (show() alone could land the cursor inside the default geometry and pass
  // pre-fix for the wrong reason).
  window.show();
  window.move(-100000, -100000);

  window.faint();
  QTRY_VERIFY(window.fadeFactor() < 0.1);

  // On the offscreen test platform the cursor is fixed at (0,0), which can
  // never fall inside the parked geometry; a WM that clamps the parked window
  // back on-screen would silently disarm the cursor-outside branch below, so
  // assert the precondition instead of letting discrimination degrade.
  QVERIFY(!window.frameGeometry().contains(QCursor::pos()));

  // Drive the poll directly: a cursor-outside tick must clear only the hover
  // source and leave the pause-faint dimmed.
  window.pollHoverHide();
  QTest::qWait(400);                  // Let any (buggy) unfade run to completion.
  QVERIFY(window.fadeFactor() < 0.1); // Poll must NOT brighten the paused window.
}

void TestLyricController::pauseHideAutoFaintsAtStartup()
{
  // Reference parity: usePauseHide.ts watches isPlay with immediate:true and
  // the store defaults isPlay=false — a freshly loaded window is treated as
  // paused and faints after 200 ms. This keeps a RESTARTED display dimmed
  // while the music is paused even before the first set_info/set_status
  // arrives (or if the host is unreachable).
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults(); // desktopLyric.pauseHide defaults to true.
  PauseHide pauseHide(ctx.config);

  QSignalSpy faintSpy(&pauseHide, &PauseHide::faintRequested);
  QTRY_COMPARE(faintSpy.count(), 1); // Fires ~200 ms after construction.
}

void TestLyricController::pauseHideStartupFaintCancelledByPlay()
{
  // The startup faint must be cancelled when playback is reported playing —
  // a playing window must not dim 200 ms after load (set_info(isPlay=true)
  // arrives during the delay).
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults();
  PauseHide pauseHide(ctx.config);

  QSignalSpy faintSpy(&pauseHide, &PauseHide::faintRequested);
  pauseHide.setPlayState(true); // Play resumed during the startup delay.
  QTest::qWait(400);            // Past the 200 ms delay.
  QCOMPARE(faintSpy.count(), 0);

  pauseHide.setPlayState(false); // Now pause: the 200 ms faint must fire.
  QTRY_COMPARE(faintSpy.count(), 1);
}

void TestLyricController::pauseHideEnableMidSessionFaintsWhenPaused()
{
  // Mid-session symmetry: enabling desktopLyric.pauseHide while playback is
  // paused must faint after the 200 ms delay — the reference outer watcher
  // re-applies the current isPlay on every enable. A bare return on enable
  // would leave the window bright until the next play-state message (which
  // never comes while paused). DesktopLyricConfig loads defaults in its ctor
  // and PauseHide reads the key in its ctor, so the disabled state must be
  // set BEFORE constructing.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults();
  QVERIFY(ctx.config.set(QStringLiteral("desktopLyric.pauseHide"), false));
  PauseHide pauseHide(ctx.config);

  QSignalSpy faintSpy(&pauseHide, &PauseHide::faintRequested);
  QVERIFY(ctx.config.set(QStringLiteral("desktopLyric.pauseHide"), true));
  QTRY_VERIFY(faintSpy.count() >= 1); // Fires ~200 ms after enable.
}

void TestLyricController::pauseHideEnableMidSessionStaysBrightWhenPlaying()
{
  // The flip side: enabling pauseHide while playback is reported PLAYING must
  // not faint. setPlayState(true) is recorded while disabled (the re-enable
  // must see the current state, not the isPlay=false default), so this guards
  // the enable branch from fainting on a stale default.
  AppContext ctx(CliOptions{});
  ctx.config.loadDefaults();
  QVERIFY(ctx.config.set(QStringLiteral("desktopLyric.pauseHide"), false));
  PauseHide pauseHide(ctx.config);

  QSignalSpy faintSpy(&pauseHide, &PauseHide::faintRequested);
  pauseHide.setPlayState(true); // Recorded while disabled; applies on enable.
  QVERIFY(ctx.config.set(QStringLiteral("desktopLyric.pauseHide"), true));
  QTest::qWait(400);              // Past the 200 ms delay.
  QVERIFY(faintSpy.count() == 0); // Enabling while playing must NOT faint.
}

QTEST_MAIN(TestLyricController)
#include "tst_controller.moc"
