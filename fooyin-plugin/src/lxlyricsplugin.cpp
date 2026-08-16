/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "lxlyricsplugin.h"

#include "lxlyricssettings.h"
#include "lyricsources.h"

#include <core/engine/enginecontroller.h>
#include <core/player/playercontroller.h>
#include <gui/guiconstants.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/settings/settingsmanager.h>

#include <QAction>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWindow>

void LxLyricsPlugin::initialise(const Fooyin::CorePluginContext& context)
{
  m_playerController = context.playerController;
  m_settingsManager = context.settingsManager;
  m_engineController = context.engine;

  // Plugin-side preferences (transport only; the standalone app owns its own
  // settings). Registered settings persist via the SettingsManager and let
  // the plugin subscribe to changes (see GuiPlugin::initialise).
  m_settingsManager->createSetting(LxLyrics::appPathKey, QString());
  m_settingsManager->createSetting(LxLyrics::autoSpawnKey, false);

  const bool spectrumAvailable = m_engineController && m_engineController->visualisationService();

  qInfo() << "[LX Lyrics] CorePlugin initialised; playerController, settingsManager, engine stored"
          << "| visualisationService:" << (spectrumAvailable ? "available" : "unavailable");
}

void LxLyricsPlugin::initialise(const Fooyin::GuiPluginContext& context)
{
  m_actionManager = context.actionManager;

  m_toggleAction = new QAction(tr("Desktop Lyrics"), this);
  m_toggleAction->setCheckable(true);
  connect(m_toggleAction, &QAction::toggled, this, &LxLyricsPlugin::toggleDesktopLyrics);

  auto* viewMenu = m_actionManager->createMenu(Fooyin::Constants::Menus::View);
  viewMenu->addAction(m_toggleAction);
  m_actionManager->registerAction(m_toggleAction,
                                  Fooyin::Id(QStringLiteral("LxLyrics.DesktopLyrics")));

  // "LX Lyrics" settings page under the Lyrics category. Constructing the
  // page with the settings dialog controller registers it (the SettingsPage
  // ctor calls SettingsDialogController::addPage). settingsDialog() is only
  // valid from GuiPlugin::initialise onwards (see settingsmanager.h).
  m_settingsPage = new LxLyricsSettingsPage(m_settingsManager, this);

  // "Open lyrics settings" button on that page: clicking it asks the running
  // lyrics app to open its own configuration dialog (protocol.md §5
  // open_settings) — so the user can reconfigure even when the lyric window
  // is locked. sendOpenSettings() is a safe no-op when the app is not
  // running; the callback dereferences m_hostServer at click time, which is
  // safe because startDesktopLyrics() creates it before the app exists and
  // stopDesktopLyrics() resets it before the app is gone.
  m_settingsPage->setOpenSettingsCallback([this] {
    if (m_hostServer != nullptr) {
      m_hostServer->sendOpenSettings();
    }
  });

  // Feed the AppSpawner on setting change (the page's apply() writes through
  // SettingsManager::set, which notifies these subscribers). The spawner is
  // also fed inside startDesktopLyrics() before every launch.
  m_settingsManager->subscribe(LxLyrics::appPathKey, this, [this](const QVariant& appPath) {
    if (m_appSpawner != nullptr) {
      m_appSpawner->setAppPath(appPath.toString());
    }
  });
  m_settingsManager->subscribe(LxLyrics::autoSpawnKey, this, [this](const QVariant& autoSpawn) {
    if (m_appSpawner != nullptr) {
      m_appSpawner->setAutoSpawn(autoSpawn.toBool());
    }
  });

  // Auto-spawn on startup when enabled. setChecked(true) flows through the
  // normal toggle path (startDesktopLyrics); deferred one event-loop turn so
  // plugin initialisation finishes first and the action reflects the state.
  if (m_settingsManager->value(LxLyrics::autoSpawnKey).toBool()) {
    QTimer::singleShot(0, this, [this] {
      m_toggleAction->setChecked(true);
    });
  }

  // Fullscreen watcher (protocol.md §5 set_fullscreen): the lyric app hides
  // behind Fooyin's MAIN window going fullscreen (reference
  // main_window_fullscreen event). The plugin lives INSIDE Fooyin's process,
  // so QGuiApplication::allWindows() reliably lists Fooyin's own windows.
  // EVERY window's windowStateChanged is observed, not just the focused
  // window's: a fullscreen window that leaves fullscreen while ANOTHER
  // window holds focus would otherwise never fire — the old focus-only
  // wiring missed that and the app stayed hidden until the next focus
  // change. Qt 6.11 has no QGuiApplication::windowAdded/windowRemoved, so
  // discovery is signal-driven: watchAllWindows() runs at startup, on every
  // focus move, and whenever any window's visibility changes; a destroyed
  // window re-checks whether the fullscreen window is gone (its
  // windowStateChanged connection auto-cleans via the context object).
  // Qt::UniqueConnection dedupes the repeated connects.
  //
  // Residual gap: a window created AND shown entirely between two scans is
  // never wired (its visibleChanged(true) fired before the connect), so if
  // it then enters fullscreen without taking focus, the lyric app stays
  // visible until the next re-scan trigger. No focus-based re-scan can
  // close this — an unwired window emits no focus signal by definition —
  // and practical impact is low: fullscreen is normally a focused-window
  // action (which re-scans first), and focusWindowChanged plus the forced
  // state send on client connect self-heal.
  if (auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    watchAllWindows(); // Seeds the tracked state before any client connects.
    // Belt-and-braces: a focus move re-scans (a new focused window gets
    // wired) and re-checks the current state.
    connect(guiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow*) {
      watchAllWindows();
    });
  }

  qInfo() << "[LX Lyrics] GuiPlugin initialised; actionManager stored; 'Desktop Lyrics' toggle "
             "added to View menu";
}

void LxLyricsPlugin::shutdown()
{
  qInfo() << "[LX Lyrics] shutdown";

  // Closing the server closes the client socket; an app spawned with
  // --exit-on-disconnect quits on its own. The spectrum source is destroyed
  // first so no analyser callbacks fire after teardown.
  if (m_playerBridge != nullptr) {
    m_playerBridge->stopPush();
  }
  m_spectrumSource.reset();
  m_playerBridge.reset();
  m_hostServer.reset();
  if (m_appSpawner != nullptr) {
    m_appSpawner->stop();
  }
  m_appSpawner.reset();
}

void LxLyricsPlugin::toggleDesktopLyrics(bool checked)
{
  if (checked) {
    startDesktopLyrics();
  } else {
    stopDesktopLyrics();
  }
}

void LxLyricsPlugin::startDesktopLyrics()
{
  if (m_hostServer == nullptr) {
    m_hostServer = std::make_unique<HostServer>();
    connect(m_hostServer.get(), &HostServer::clientConnected, this, [this] {
      qInfo() << "[LX Lyrics] lyrics app connected";
      // Force the current fullscreen state on connect: a freshly spawned app
      // must start correctly hidden when Fooyin is already fullscreen (the
      // changed-only watcher would otherwise never send anything).
      updateFullscreen(true);
    });
    connect(m_hostServer.get(), &HostServer::clientDisconnected, this,
            &LxLyricsPlugin::onClientDisconnected);
    // A protocol-error close is NOT a crash: clear stale spawner
    // bookkeeping only (the app exits on the socket close via
    // --exit-on-disconnect), keep the server listening for a corrected
    // client, and never schedule the respawn loop.
    connect(m_hostServer.get(), &HostServer::protocolErrorClosed, this,
            &LxLyricsPlugin::onProtocolErrorClosed);
    // The user closed the lyric window in the app: end the session without
    // respawning (the disconnect that follows must not look like a crash).
    connect(m_hostServer.get(), &HostServer::closeRequested, this,
            &LxLyricsPlugin::onCloseRequested);

    m_playerBridge = std::make_unique<PlayerBridge>(m_playerController, m_hostServer.get(), this);
    // Lyric acquisition: raw embedded tags / local .lrc files only. The
    // display app owns all parsing/selection (docs/protocol.md §1); the
    // plugin never sees beyond these four opaque strings.
    m_playerBridge->setLyricProvider(
      [](const Fooyin::Track& track, QString& lrc, QString& tlrc, QString& rlrc, QString& lxlrc) {
        const LxLyrics::LyricsResult result = LxLyrics::LyricSource::fetch(track);
        lrc = result.lrc;
        tlrc = result.tlrc;
        rlrc = result.rlrc;
        lxlrc = result.lxlrc;
      });
    connect(m_hostServer.get(), &HostServer::clientConnected, m_playerBridge.get(),
            &PlayerBridge::onClientConnected);
    connect(m_hostServer.get(), &HostServer::clientDisconnected, m_playerBridge.get(),
            &PlayerBridge::stopPush);
    connect(m_hostServer.get(), &HostServer::requestInfo, m_playerBridge.get(),
            &PlayerBridge::handleRequestInfo);
    connect(m_hostServer.get(), &HostServer::requestStatus, m_playerBridge.get(),
            &PlayerBridge::handleRequestStatus);
    connect(m_hostServer.get(), &HostServer::requestAnalyserData, m_playerBridge.get(),
            &PlayerBridge::handleRequestAnalyserData);

    // Spectrum: PlayerBridge forwards each analyser request as
    // analyserDataRequested (it gates on the pushing state); SpectrumSource
    // pulls a fresh frame and replies over the same HostServer.
    m_spectrumSource =
      std::make_unique<SpectrumSource>(m_engineController, m_hostServer.get(), this);
    connect(m_playerBridge.get(), &PlayerBridge::analyserDataRequested, m_spectrumSource.get(),
            &SpectrumSource::onAnalyserDataRequested);
  }

  if (!m_hostServer->isListening()) {
    qWarning() << "[LX Lyrics] desktop lyrics server not listening; cannot start app";
    m_toggleAction->setChecked(false);
    return;
  }

  if (m_appSpawner == nullptr) {
    m_appSpawner = std::make_unique<AppSpawner>();
  }
  applySpawnerSettings(); // path + auto-spawn from settings before every launch

  // Idempotency guard: the app may already be running (e.g. the init-time
  // auto-spawn followed by a manual toggle click, or a double invocation).
  // Skipping the early return would spawn a SECOND detached lyrics-app.
  // When the app has exited on its own (crashed / socket closed) isRunning()
  // is false and the spawn below proceeds normally.
  if (m_appSpawner->isRunning()) {
    qInfo() << "[LX Lyrics] lyrics-app already running, skipping duplicate spawn";
    return;
  }

  // Forced spawn: both callers (manual View-menu toggle and the init-time
  // auto-spawn) only run when the user wants the app up; the AutoSpawn
  // setting key already gates whether the auto path fires at all.
  if (!m_appSpawner->spawn(serverWsUrl(), true)) {
    qWarning() << "[LX Lyrics] failed to start lyrics app; disabling desktop lyrics";
    m_toggleAction->setChecked(false);
  }
}

void LxLyricsPlugin::stopDesktopLyrics()
{
  if (m_playerBridge != nullptr) {
    m_playerBridge->stopPush();
  }
  // Spectrum source first: no analyser callbacks fire after teardown.
  m_spectrumSource.reset();
  m_playerBridge.reset();

  if (m_hostServer == nullptr) {
    return;
  }

  qInfo() << "[LX Lyrics] desktop lyrics disabled; closing server (app exits on socket close)";
  m_hostServer.reset();
  if (m_appSpawner != nullptr) {
    m_appSpawner->stop();
  }
}

void LxLyricsPlugin::onCloseRequested()
{
  // protocol.md §4: the user intentionally closed the lyric window. End the
  // desktop-lyrics session like a View-menu toggle-off — the action must read
  // unchecked IMMEDIATELY so the disconnect that follows early-exits
  // onClientDisconnected (its toggle check) and can never enter the
  // crash-recovery respawn path. The teardown itself is DEFERRED one event
  // loop turn: this slot runs synchronously inside the emitting
  // HostServer::onTextMessageReceived, and the normal toggle path would
  // destroy m_hostServer — the signal's own sender — mid-emission.
  // QSignalBlocker suppresses the synchronous toggleDesktopLyrics teardown;
  // the queued callback below performs it once the emission has fully
  // returned. Idempotent: a duplicate frame schedules a second callback that
  // early-exits on the identity guard (the server is already gone).
  if (m_toggleAction != nullptr) {
    const QSignalBlocker blocker(m_toggleAction);
    m_toggleAction->setChecked(false);
  }

  // Capture the emitting server: the queued teardown must only destroy the
  // server that actually closed. If the user manually toggled the action
  // back ON while we waited (reusing the still-alive server) or the
  // lifecycle replaced the server (toggle off/on), the session is no longer
  // the one that closed and must be left alone.
  QPointer<HostServer> closingHost = m_hostServer.get();
  QTimer::singleShot(0, this, [this, closingHost] {
    if (m_toggleAction != nullptr && m_toggleAction->isChecked())
      return; // The user re-enabled desktop lyrics while we waited.
    if (m_hostServer.get() != closingHost)
      return; // Replaced (or already torn down): the new session owns its lifecycle.
    stopDesktopLyrics();
  });
}

void LxLyricsPlugin::onProtocolErrorClosed()
{
  // A connected client was closed for a protocol violation (hostserver.cpp):
  // this is a malformed-client event, NOT a crash. Keep the server listening
  // so a corrected client may reconnect; only clear the spawner's stale
  // running flag — the detached app exits on its own when the socket closes
  // (--exit-on-disconnect) and must not block a later manual spawn with
  // "already running". No respawn loop, no toggle change. Null-safe and
  // idempotent.
  qWarning() << "[LX Lyrics] client closed for protocol error; keeping server listening";
  if (m_appSpawner != nullptr) {
    m_appSpawner->stop();
  }
}

void LxLyricsPlugin::onClientDisconnected()
{
  if (m_toggleAction == nullptr || !m_toggleAction->isChecked()) {
    return;
  }
  if (m_hostServer == nullptr || m_appSpawner == nullptr) {
    return;
  }

  // protocol.md §2: the host may respawn the app after a disconnect. The
  // server keeps listening on the same port, so the URL is unchanged.
  // Guard against stale servers (task D): the user may toggle off/on during
  // the 1500 ms delay, replacing m_hostServer with a NEW server on a NEW
  // port; the old timer must never spawn against it. Capture the emitting
  // instance and the URL at schedule time, and require the current server to
  // still BE that instance before acting.
  QPointer<HostServer> disconnectedHost = m_hostServer.get();
  const QUrl url = serverWsUrl();

  qInfo() << "[LX Lyrics] app disconnected; respawning in 1500 ms";
  QTimer::singleShot(1500, this, [this, disconnectedHost, url] {
    if (m_hostServer.get() != disconnectedHost) {
      // The server was replaced while waiting (toggle off/on): drop the
      // stale respawn — the new session owns its own lifecycle.
      return;
    }
    if (!m_toggleAction->isChecked() || m_hostServer == nullptr || !m_hostServer->isListening()) {
      return;
    }
    m_appSpawner->stop();
    // Respawn is a manual path too: it only fires while the toggle is
    // checked, so the AutoSpawn key must not block it.
    m_appSpawner->spawn(url, true);
  });
}

QUrl LxLyricsPlugin::serverWsUrl() const
{
  QUrl url;
  url.setScheme(QStringLiteral("ws"));
  url.setHost(QStringLiteral("127.0.0.1"));
  url.setPort(m_hostServer->serverPort());
  return url;
}

void LxLyricsPlugin::applySpawnerSettings()
{
  if (m_appSpawner == nullptr || m_settingsManager == nullptr) {
    return;
  }
  m_appSpawner->setAppPath(m_settingsManager->value(LxLyrics::appPathKey).toString());
  m_appSpawner->setAutoSpawn(m_settingsManager->value(LxLyrics::autoSpawnKey).toBool());
}

void LxLyricsPlugin::watchAllWindows()
{
  const QList<QWindow*> windows = QGuiApplication::allWindows();
  for (QWindow* window : windows) {
    // UniqueConnection + this as the connection context: repeated connects
    // dedupe, and destroyed windows auto-clean their connections. The slots
    // are member functions, not lambdas: Qt::UniqueConnection with a functor
    // slot asserts in Debug builds (Qt 6.11 qobject.h).
    connect(window, &QWindow::windowStateChanged, this, &LxLyricsPlugin::onWindowStateChanged,
            Qt::UniqueConnection);
    connect(window, &QWindow::visibleChanged, this, &LxLyricsPlugin::onWindowVisibleChanged,
            Qt::UniqueConnection);
    connect(window, &QObject::destroyed, this, &LxLyricsPlugin::onWindowDestroyed,
            Qt::UniqueConnection);
  }
  updateFullscreen();
}

void LxLyricsPlugin::onWindowStateChanged()
{
  updateFullscreen();
}

void LxLyricsPlugin::onWindowVisibleChanged()
{
  watchAllWindows(); // A newly visible window may not have been wired yet.
}

void LxLyricsPlugin::onWindowDestroyed()
{
  updateFullscreen(); // The removed window may have been the fullscreen one.
}

void LxLyricsPlugin::updateFullscreen(bool force)
{
  // Fooyin's own windows (the plugin lives in its process): any window in
  // the fullscreen state counts — the lyric app must hide behind the main
  // window's fullscreen (protocol.md §5 set_fullscreen, reference
  // main_window_fullscreen event).
  bool isFullscreen = false;
  const QList<QWindow*> windows = QGuiApplication::allWindows();
  for (QWindow* window : windows) {
    // windowStates() carries the QFlags (QWindow::windowState() alone
    // returns a single Qt::WindowState in this Qt version).
    if (window->windowStates().testFlag(Qt::WindowFullScreen)) {
      isFullscreen = true;
      break;
    }
  }

  if (!force && m_lastFullscreenSent.has_value() && m_lastFullscreenSent.value() == isFullscreen) {
    return; // Unchanged state: nothing to report.
  }
  m_lastFullscreenSent = isFullscreen;
  if (m_hostServer != nullptr) {
    m_hostServer->sendSetFullscreen(isFullscreen);
  }
}
