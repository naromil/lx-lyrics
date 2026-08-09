/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QApplication>
#include <QCoreApplication>
#include <QUrl>
#include <QtGlobal>

#include <memory>

#include "app/appcontext.h"
#include "app/clioptions.h"
#include "app/lyriccontroller.h"
#include "app/spectrumbridge.h"
#include "bridge/wsclient.h"
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
  track.id = QStringLiteral("demo-track");
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
  track.line = -1;
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

  QApplication app(argc, argv);

  const CliOptions cli = parseCliOptions(QApplication::arguments());

  AppContext appContext(cli);
  appContext.config.load(); // Apply persisted settings before the window exists.

  // i18n after load() so a persisted common.langId is applied at startup;
  // subsequent changes are picked up through DesktopLyricConfig::settingChanged.
  appContext.i18n = std::make_unique<TranslationManager>(appContext.config, &appContext);

  auto* window = new LyricWindow(appContext.config, *appContext.i18n);
  appContext.mainWindow = window;

  // Assemble the full lyric pipeline (task 2.13): LyricSelector -> LyricPlayer
  // -> LyricRenderer inside the window, live-synced to config. Shared by both
  // modes below (host-driven and --demo self-feed).
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

  // WebSocket bridge to the host plugin (docs/protocol.md). Every parsed host
  // message is routed into the music-state machine (task 2.13): the lyric
  // pipeline in LyricController, the play boolean into PauseHide, and the
  // spectrum frames into SpectrumBridge. Reconnect is owned by WsClient.
  if (!cli.wsUrl.isEmpty()) {
    appContext.wsClient =
      std::make_unique<WsClient>(WsClient::kDefaultReconnectIntervalMs, &appContext);
    WsClient* ws = appContext.wsClient.get();

    QObject::connect(ws, &WsClient::connected, &appContext, [ws] {
      qInfo() << "ws: connected, requesting track info";
      ws->sendGetInfo(); // Mirrors the reference init() -> getInfo().
    });
    QObject::connect(ws, &WsClient::disconnected, &appContext, [&appContext] {
      qInfo() << "ws: disconnected from host";
      if (appContext.cli.exitOnDisconnect)
        QCoreApplication::quit();
    });

    QObject::connect(ws, &WsClient::infoReceived, &appContext,
                     [&appContext, ws, lyricController](const TrackSnapshot& info) {
                       lyricController->setTrack(info);
                       appContext.pauseHide->setPlayState(info.isPlay);
                       // The host may not push a fresh set_status after a
                       // track change; ask for one so playback starts on
                       // the new lyric (reference set_info -> getStatus).
                       if (info.isPlay)
                         ws->sendGetStatus();
                     });
    QObject::connect(ws, &WsClient::lyricReceived, &appContext,
                     [lyricController](const LyricSnapshot& lyric) {
                       lyricController->setLyric(lyric);
                     });
    QObject::connect(ws, &WsClient::statusReceived, &appContext,
                     [&appContext, lyricController](const PlaybackSnapshot& status) {
                       lyricController->setStatus(status);
                       appContext.pauseHide->setPlayState(status.isPlay);
                     });
    QObject::connect(ws, &WsClient::offsetReceived, &appContext,
                     [lyricController](qint64 tempOffset) {
                       lyricController->setOffset(tempOffset);
                     });
    QObject::connect(ws, &WsClient::playbackRateReceived, &appContext,
                     [lyricController](double rate) {
                       lyricController->setPlaybackRate(rate);
                     });
    QObject::connect(ws, &WsClient::playReceived, &appContext,
                     [&appContext, lyricController](qint64 timeMs) {
                       lyricController->play(timeMs);
                       appContext.pauseHide->setPlayState(true);
                     });
    QObject::connect(ws, &WsClient::pauseReceived, &appContext, [&appContext, lyricController] {
      lyricController->pause();
      appContext.pauseHide->setPlayState(false);
    });
    QObject::connect(ws, &WsClient::stopReceived, &appContext, [&appContext, lyricController] {
      lyricController->stop();
      appContext.pauseHide->setPlayState(false);
    });

    // Host control messages (§5): open_settings raises the app's own
    // configuration dialog — the same path as Ctrl+, / the control-bar gear
    // button — so the host can reconfigure even a locked lyric window.
    QObject::connect(ws, &WsClient::openSettingsRequested, window,
                     &LyricWindow::openSettingsDialog);

    // Spectrum visualizer wiring (task 2.11, unchanged): the bridge owns
    // all spectrum-only couplings (frames -> widget, requests -> host, the
    // (isPlay && audioVisualization) gate). The controller does NOT own it.
    appContext.spectrumBridge = std::make_unique<SpectrumBridge>(window->spectrumWidget(), ws,
                                                                 appContext.config, &appContext);

    ws->connectToHost(QUrl(cli.wsUrl));
  }

  // Standalone self-feed (task 2.13): the fake track goes through the SAME
  // pipeline as a host set_info. play(0) starts the player's own clock and
  // the player's lineChanged signal moves the renderer's active line — no
  // WsClient and no SpectrumBridge are constructed, so audioVisualization
  // stays off.
  if (cli.demo) {
    const TrackSnapshot demo = makeDemoTrack();
    lyricController->setTrack(demo);
    lyricController->play(0);
    window->setWindowTitle(QStringLiteral("DEMO: %1 - %2").arg(demo.name, demo.singer));
  }

  window->show();

  return QApplication::exec();
}
