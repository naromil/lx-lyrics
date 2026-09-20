/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QApplication>
#include <QCoreApplication>
#include <QtGlobal>

#include <csignal>
#include <memory>

#include "app/appcontext.h"
#include "app/clioptions.h"
#include "app/lyriccontroller.h"
#include "app/spectrumbridge.h"
#include "app/spectrumtransport.h"
#include "host/feedreader.h"
#include "host/pipeio.h"
#include "i18n/translationmanager.h"
#include "window/lyricwindow.h"

namespace {

// Fake track for --demo: a multi-line lyric that displays line-by-line (active
// line = played color, inactive = unplay). Most lines are plain; one main line
// and one extended (tlrc) line keep karaoke word tags to prove the renderer's
// stripWordTags runs on both the main- and extended-line paths. It flows
// through the SAME pipeline as a host set_info message
// (LyricController::setTrack -> LyricPlayer -> LyricRenderer), so the demo
// exercises the full render path.
TrackSnapshot makeDemoTrack()
{
  TrackSnapshot track;
  track.singer = QStringLiteral("Demo Singer");
  track.name = QStringLiteral("Demo Song");
  track.album = QStringLiteral("Demo Album");
  track.lrc = QStringLiteral("[00:00.00]Demo line one\n"
                             "[00:02.00]<0,1500>This <1500,1500>line has word tags\n"
                             "[00:05.00]Plain third line");
  track.tlrc = QStringLiteral("[00:00.00]First line translation\n"
                              "[00:02.00]<0,1200>醒目 <1200,800>金色\n"
                              "[00:05.00]Third line translation");
  track.rlrc = QStringLiteral("[00:00.00]First rōmaji\n"
                              "[00:02.00]Tagged rōmaji\n"
                              "[00:05.00]Third rōmaji");
  track.lxlrc = QString();
  track.isPlay = true;
  track.playedTimeMs = 0;
  return track;
}

} // namespace

int main(int argc, char* argv[])
{
  // Force the X11 (xcb) platform: client-side window positioning (move()) is
  // only honored by X11; Wayland compositors ignore client-set positions,
  // breaking position restore. Respect an explicit user override; fall back
  // to the platform default otherwise.
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));

  // Qt >= 6.11 drops qInfo/qWarning/qCritical output when stderr is not a
  // terminal — but this app's stderr is a PIPE by design (docs/protocol.md
  // §2: the player-side adapter captures it and forwards the lines into the
  // player's log), and stdout is reserved for protocol lines. Force the
  // logging on so a spawned child is not silent; respect an explicit override.
  if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING"))
    qputenv("QT_FORCE_STDERR_LOGGING", QByteArrayLiteral("1"));

  // A host may drop the app's stdout while its stdin lives (a player whose
  // read loop died, a plugin teardown that closes the read channel first), and
  // a bare ::write() to a pipe with no reader both raises SIGPIPE — whose
  // default action kills this process before ::write can return EPIPE — and
  // fails with EPIPE. Ignore it here, before any feed device exists, so the
  // failure surfaces as a dropped line instead of a death the adapter would
  // read as a crash. Inert for the non-feed modes: they never write to a pipe.
  std::signal(SIGPIPE, SIG_IGN);

  QApplication app(argc, argv);

  const CliOptions cli = parseCliOptions(QApplication::arguments());

  AppContext appContext(cli);
  appContext.config.load(); // Apply persisted settings before the window exists.

  // i18n after load() so a persisted common.langId is applied at startup;
  // subsequent changes are picked up through DesktopLyricConfig::settingChanged.
  appContext.i18n = std::make_unique<TranslationManager>(appContext.config, &appContext);

  auto* window = new LyricWindow(appContext.config, *appContext.i18n);
  appContext.mainWindow = window;

  // Close animation: the X button fades the content out (300 ms, inside
  // LyricWindow), then this signal quits the app. Other quit paths (the host
  // closing the feed pipe, WM close) stay instant.
  QObject::connect(window, &LyricWindow::closeAnimationFinished, &app, &QCoreApplication::quit);

  // Assemble the full lyric pipeline (task 2.13): LyricSelector -> LyricPlayer
  // -> LyricRenderer inside the window, live-synced to config. Shared by both
  // modes below (player feed and --demo self-feed).
  auto* lyricController = new LyricController(appContext, *window, &appContext);

  // Pause-faint watcher (port of usePauseHide.ts): the play boolean carried by
  // any state message decides whether the lyric window dims on pause. The
  // window is NEVER hidden or shown here — only content-faded via
  // LyricWindow::faint/unfaint (CSS-style opacity, not window opacity).
  appContext.pauseHide = std::make_unique<PauseHide>(appContext.config, &appContext);
  QObject::connect(appContext.pauseHide.get(), &PauseHide::faintRequested, window,
                   &LyricWindow::faint);
  QObject::connect(appContext.pauseHide.get(), &PauseHide::unfaintRequested, window,
                   &LyricWindow::unfaint);

  // Player feed (docs/protocol.md v2): with --player-feed the app is the
  // DIRECT CHILD of a player-side adapter and speaks newline-delimited JSON
  // over its stdin (host→app) and stdout (app→host). QFile cannot serve
  // either direction — Qt's file engine reports no readiness and no bytes for
  // a pipe, so the reader would never see a message — hence the pipe devices
  // in host/pipeio.h. Every parsed host message is routed into the music-state
  // machine: the lyric pipeline in LyricController, the play boolean into
  // PauseHide, the spectrum frames into SpectrumBridge.
  if (cli.playerFeed) {
    auto* stdinDevice = new PipeReadDevice(0, &appContext);
    auto* stdoutDevice = new PipeWriteDevice(1, &appContext);
    appContext.feedReader = std::make_unique<FeedReader>(stdinDevice, stdoutDevice, &appContext);
    FeedReader* feed = appContext.feedReader.get();

    QObject::connect(feed, &FeedReader::infoReceived, &appContext,
                     [&appContext, lyricController](const TrackSnapshot& info) {
                       lyricController->setTrack(info);
                       appContext.pauseHide->setPlayState(info.isPlay);
                     });
    QObject::connect(feed, &FeedReader::lyricReceived, &appContext,
                     [lyricController](const LyricSnapshot& lyric) {
                       lyricController->setLyric(lyric);
                     });
    QObject::connect(feed, &FeedReader::statusReceived, &appContext,
                     [&appContext, lyricController](const PlaybackSnapshot& status) {
                       lyricController->setStatus(status);
                       appContext.pauseHide->setPlayState(status.isPlay);
                     });
    QObject::connect(feed, &FeedReader::offsetReceived, &appContext,
                     [lyricController](qint64 tempOffset) {
                       lyricController->setOffset(tempOffset);
                     });
    QObject::connect(feed, &FeedReader::playbackRateReceived, &appContext,
                     [lyricController](double rate) {
                       lyricController->setPlaybackRate(rate);
                     });
    QObject::connect(feed, &FeedReader::playReceived, &appContext,
                     [&appContext, lyricController](qint64 timeMs) {
                       lyricController->play(timeMs);
                       appContext.pauseHide->setPlayState(true);
                     });
    QObject::connect(feed, &FeedReader::pauseReceived, &appContext, [&appContext, lyricController] {
      lyricController->pause();
      appContext.pauseHide->setPlayState(false);
    });
    QObject::connect(feed, &FeedReader::stopReceived, &appContext, [&appContext, lyricController] {
      lyricController->stop();
      appContext.pauseHide->setPlayState(false);
    });

    // Host control messages (§5): open_settings raises the app's own
    // configuration dialog — the same path as Ctrl+, / the control-bar gear
    // button — so the host can reconfigure even a locked lyric window.
    QObject::connect(feed, &FeedReader::openSettingsRequested, window,
                     &LyricWindow::openSettingsDialog);

    // Host fullscreen state (§5 set_fullscreen): while desktopLyric.
    // fullscreenHide is enabled the lyric window hides when the host's main
    // window enters fullscreen and shows again when it leaves.
    QObject::connect(feed, &FeedReader::fullscreenReceived, window,
                     &LyricWindow::setHostFullscreen);

    // User-initiated window close (control-bar X or WM close, once per
    // session): tell the player so it ends its session WITHOUT respawning the
    // app (§4 close_requested). The host's handler is idempotent, and the
    // player closes the pipe right after — the EOF below ends this process.
    QObject::connect(window, &LyricWindow::closeInitiated, &appContext, [feed] {
      feed->sendCloseRequested();
    });

    // §2: EOF on stdin means the player is gone. Quit immediately (the close
    // animation is the app's own X-button path, not this one).
    QObject::connect(feed, &FeedReader::exited, &appContext, [] {
      QCoreApplication::quit();
    });

    // §7: a protocol violation is fatal — log loudly and exit non-zero rather
    // than half-render data the host may not have meant.
    QObject::connect(feed, &FeedReader::protocolError, &appContext, [](const QString& reason) {
      qCritical() << "feed: protocol error, exiting:" << reason;
      QCoreApplication::exit(1);
    });

    // Spectrum visualizer wiring: the bridge owns every spectrum-only
    // coupling (frames -> widget, requests -> host, the (playing &&
    // audioVisualization) gate); the controller does NOT own it. The transport
    // seam keeps the feed path and the --demo self-feed on one gate and one
    // request loop, and suppresses requests while the host declared no
    // analyser.
    appContext.spectrumTransport = std::make_unique<FeedSpectrumTransport>(feed, &appContext);
    appContext.spectrumBridge = std::make_unique<SpectrumBridge>(
      window->spectrumWidget(), appContext.spectrumTransport.get(), appContext.config, &appContext);
  }

  // Standalone self-feed (task 2.13): the fake track goes through the SAME
  // pipeline as a host set_info. play(0) starts the player's own clock and
  // the player's lineChanged signal moves the renderer's active line — no
  // FeedReader is constructed. The visualizer is fed by the synthetic
  // transport below (a hostless demo has no host to ask), through the same
  // bridge and gate a host drives.
  else if (cli.demo) {
    const TrackSnapshot demo = makeDemoTrack();
    lyricController->setTrack(demo);
    lyricController->play(0);
    appContext.pauseHide->setPlayState(true); // The demo plays: keep it bright.
    window->setWindowTitle(QStringLiteral("DEMO: %1 - %2").arg(demo.name, demo.singer));

    appContext.spectrumTransport = std::make_unique<DemoSpectrumTransport>(&appContext);
    appContext.spectrumBridge = std::make_unique<SpectrumBridge>(
      window->spectrumWidget(), appContext.spectrumTransport.get(), appContext.config, &appContext);
  }

  // Show path split (host-visibility): the DEMO always displays — it must
  // prove the render path even when the config would hide the window
  // (enable=false, or host fullscreen + fullscreenHide). The host path
  // re-runs the host-visible conditions instead of show(), so a window that
  // must stay hidden (desktopLyric.enable off at startup, or the host
  // already fullscreen) never flashes on screen once.
  if (cli.demo)
    window->show();
  else
    window->updateHiddenByHostConditions();

  return QApplication::exec();
}
