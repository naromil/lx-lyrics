/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "lxlyricsplugin.h"

#include "lxlyricssettings.h"

#include <core/engine/enginecontroller.h>
#include <core/player/playercontroller.h>
#include <gui/guiconstants.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/settings/settingsmanager.h>

#include <QAction>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

void LxLyricsPlugin::initialise(const Fooyin::CorePluginContext& context)
{
  m_playerController = context.playerController;
  m_settingsManager = context.settingsManager;
  m_engineController = context.engine;

  // Plugin-side preferences (transport only; the standalone app owns its own
  // settings). Registered settings persist via the SettingsManager and let
  // the plugin subscribe to changes (see GuiPlugin::initialise). Enabled is
  // plugin-written state, not a user preference: rememberState() records the
  // last desktop-lyrics on/off state there so startup can restore it.
  m_settingsManager->createSetting(LxLyrics::appPathKey, QString());
  m_settingsManager->createSetting(LxLyrics::rememberStateKey, true);
  m_settingsManager->createSetting(LxLyrics::enabledKey, false);
  m_settingsManager->createSetting(LxLyrics::hotkeyKey, QStringLiteral("CTRL+ALT+l"));
  m_settingsManager->createSetting(LxLyrics::hotkeyHelperKey, QString());

  const bool spectrumAvailable = m_engineController && m_engineController->visualisationService();

  qInfo() << "[LX Lyrics] CorePlugin initialised; playerController, settingsManager, engine stored"
          << "| visualisationService:" << (spectrumAvailable ? "available" : "unavailable");

  // The helper needs no player interface and no window session: it is started
  // here so the key exists for the plugin's whole lifetime (enabled ≠ showing).
  startHotkey();
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

  // Config surface: the plugin's own dialog, offered through
  // PluginConfigGuiPlugin (see settingsProvider) — Fooyin's dedicated
  // per-plugin configuration place: the Plugins page's "Configure" button.
  // The host builds the provider once at startup and owns it; the dialog
  // itself is created on first Configure click.

  // Feed the FeedWriter on setting change (the dialog's accept() writes
  // through SettingsManager::set, which notifies this subscriber). The writer
  // is also fed inside startDesktopLyrics() before every launch.
  m_settingsManager->subscribe(LxLyrics::appPathKey, this, [this](const QVariant& appPath) {
    if (m_feedWriter != nullptr) {
      m_feedWriter->setAppPath(appPath.toString());
    }
  });

  // Restore the remembered desktop-lyrics state unless remembering is off.
  // setChecked(true) flows through the normal toggle path
  // (startDesktopLyrics), so the app spawns exactly like a manual toggle;
  // deferred one event-loop turn so plugin initialisation finishes first and
  // the action reflects the state. RememberState=false always starts off,
  // regardless of the persisted Enabled value.
  if (m_settingsManager->value(LxLyrics::rememberStateKey).toBool() &&
      m_settingsManager->value(LxLyrics::enabledKey).toBool()) {
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
  // state send on app start self-heal.
  if (auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    watchAllWindows(); // Seeds the tracked state before any app starts.
    // Belt-and-braces: a focus move re-scans (a new focused window gets
    // wired) and re-checks the current state.
    connect(guiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow*) {
      watchAllWindows();
    });
  }

  qInfo() << "[LX Lyrics] GuiPlugin initialised; actionManager stored; 'Desktop Lyrics' toggle "
             "added to View menu";
}

std::unique_ptr<Fooyin::PluginSettingsProvider> LxLyricsPlugin::settingsProvider() const
{
  // Called once by the host during GuiApplication::initialise, which runs after
  // CorePlugin::initialise — m_settingsManager is set by then, and the dialog
  // dereferences it. The callback dereferences m_feedWriter at click time,
  // which is safe because startDesktopLyrics() creates the writer before any
  // app exists and it outlives every child (stopDesktopLyrics() only stops the
  // child); sendOpenSettings() is itself a no-op while no child runs.
  qInfo() << "[LX Lyrics] config provider created for the Plugins page's Configure button";
  return std::make_unique<LxLyricsPluginSettingsProvider>(m_settingsManager, [this] {
    if (m_feedWriter != nullptr) {
      m_feedWriter->sendOpenSettings();
    }
  });
}

void LxLyricsPlugin::shutdown()
{
  qInfo() << "[LX Lyrics] shutdown";

  // The helper goes first: its reader thread must not queue a toggle into a
  // session that is being torn down.
  stopHotkey();

  // Stopping the feed terminates the child lyrics-app: the lyric window lives
  // and dies with the session, never as an orphan. The spectrum source is
  // destroyed first so no analyser callbacks fire after teardown.
  teardownSession();
  if (m_feedWriter != nullptr) {
    m_feedWriter->stop();
  }
  m_feedWriter.reset();
}

void LxLyricsPlugin::toggleDesktopLyrics(bool checked)
{
  // Record the state first — on both flips — so the next session can restore
  // it. RememberState only gates the startup restore, never this write.
  rememberState(checked);

  if (checked) {
    startDesktopLyrics();
  } else {
    stopDesktopLyrics();
  }
}

void LxLyricsPlugin::rememberState(bool enabled)
{
  if (m_settingsManager == nullptr) {
    return;
  }
  m_settingsManager->set(LxLyrics::enabledKey, enabled);
}

void LxLyricsPlugin::startDesktopLyrics()
{
  if (m_feedWriter == nullptr) {
    m_feedWriter = std::make_unique<FeedWriter>();
    // The child is up and its hello line has been written: force the current
    // fullscreen state on start — a freshly spawned app must start correctly
    // hidden when Fooyin is already fullscreen (the changed-only watcher
    // would otherwise never send anything) — and push the initial snapshot
    // (v2 has no get_info; the pipe buffers make this race-free).
    connect(m_feedWriter.get(), &FeedWriter::appStarted, this, [this] {
      qInfo() << "[LX Lyrics] lyrics app started";
      updateFullscreen(true);
      if (m_playerBridge != nullptr) {
        m_playerBridge->onAppStarted();
      }
    });
    // The child ended: a clean exit without a close request is a crash to
    // recover from, a non-zero status is a protocol abort that ends the
    // session (see onAppExited).
    connect(m_feedWriter.get(), &FeedWriter::appExited, this, &LxLyricsPlugin::onAppExited);
    // The user closed the lyric window in the app: end the session without
    // respawning (the exit that follows must not look like a crash).
    connect(m_feedWriter.get(), &FeedWriter::closeRequested, this,
            &LxLyricsPlugin::onCloseRequested);
  }

  applyFeedWriterSettings(); // app path from settings before every launch

  // The hello line declares analyser availability (protocol.md §5): the app
  // only requests frames when it is true, so tie it to the engine's
  // visualisation service before every spawn.
  const bool spectrumAvailable =
    m_engineController != nullptr && m_engineController->visualisationService() != nullptr;
  m_feedWriter->setSpectrumAvailable(spectrumAvailable);

  // Idempotency guard: the app may already be running (e.g. the startup
  // restore followed by a manual toggle click, or a double invocation).
  // Skipping the early return would spawn a SECOND lyrics-app. When the app
  // has exited on its own (crash) isRunning() is false and the spawn below
  // proceeds normally.
  if (m_feedWriter->isRunning()) {
    qInfo() << "[LX Lyrics] lyrics-app already running, skipping duplicate spawn";
    return;
  }

  // Every caller (manual View-menu toggle / startup restore, and the
  // crash-recovery respawn) only runs when the desktop lyrics are wanted.
  ensureSessionObjects(); // A start always brings the session objects with it.
  if (!m_feedWriter->spawn()) {
    qWarning() << "[LX Lyrics] failed to start lyrics app; disabling desktop lyrics";
    m_toggleAction->setChecked(false);
  }
}

void LxLyricsPlugin::ensureSessionObjects()
{
  if (m_feedWriter == nullptr || m_playerBridge != nullptr) {
    return; // No writer yet, or the live session already owns its objects.
  }

  // Playback mapping only: the app owns lyric acquisition in v2, so the
  // bridge sends playing context (path, metadata, state, position). The
  // settings manager lets it drop the stop Fooyin issues while quitting
  // (Settings::Core::Shutdown), which would otherwise clear the app's lyric
  // and make the window flicker "No lyrics" just before it closes.
  m_playerBridge =
    std::make_unique<PlayerBridge>(m_playerController, m_feedWriter.get(), m_settingsManager, this);
  // The child's analyser request is gated by the bridge's pushing state
  // (nothing is pushed to a dead or absent child).
  connect(m_feedWriter.get(), &FeedWriter::analyserDataRequested, m_playerBridge.get(),
          &PlayerBridge::handleRequestAnalyserData);

  // Spectrum: PlayerBridge forwards each analyser request as
  // analyserDataRequested (it gates on the pushing state); SpectrumSource
  // pulls a fresh frame and replies through the same feed.
  m_spectrumSource = std::make_unique<SpectrumSource>(m_engineController, m_feedWriter.get(), this);
  connect(m_playerBridge.get(), &PlayerBridge::analyserDataRequested, m_spectrumSource.get(),
          &SpectrumSource::onAnalyserDataRequested);
}

void LxLyricsPlugin::teardownSession()
{
  if (m_playerBridge != nullptr) {
    m_playerBridge->stopPush();
  }
  // Spectrum source first: no analyser callbacks fire after teardown.
  m_spectrumSource.reset();
  m_playerBridge.reset();
}

void LxLyricsPlugin::stopDesktopLyrics()
{
  teardownSession();

  if (m_feedWriter == nullptr) {
    return;
  }

  qInfo() << "[LX Lyrics] desktop lyrics disabled; stopping the lyrics app";
  // Terminates the child and waits for it, then clears the running state.
  // The writer itself stays alive (only the child is session-scoped) so the
  // AppPath subscription and the next spawn keep working.
  m_feedWriter->stop();
}

void LxLyricsPlugin::onCloseRequested()
{
  // protocol.md §4: the user intentionally closed the lyric window. End the
  // desktop-lyrics session like a View-menu toggle-off — the action must read
  // unchecked IMMEDIATELY so the exit that follows early-exits onAppExited
  // (its toggle check) and can never enter the crash-recovery respawn path.
  // The teardown itself is DEFERRED one event loop turn: this slot runs
  // synchronously inside the emitting FeedWriter's stdout handler, and the
  // normal toggle path would stop — and wait for — the child whose signal
  // emission we are inside. QSignalBlocker suppresses the synchronous
  // toggleDesktopLyrics teardown; the queued callback below performs it once
  // the emission has fully returned. Idempotent: a duplicate frame schedules
  // a second callback that early-exits on the identity guard (the writer is
  // the same, but the session teardown already ran).
  if (m_toggleAction != nullptr) {
    const QSignalBlocker blocker(m_toggleAction);
    m_toggleAction->setChecked(false);
  }
  // The blocker above suppresses toggled(), so toggleDesktopLyrics() never
  // runs to record the off state; the remembered state must not survive this
  // intentional close as "on".
  rememberState(false);

  // Capture the emitting writer: the queued teardown must only stop the
  // session that actually closed. If the user manually toggled the action
  // back ON while we waited (starting a new child through the same writer),
  // the session is no longer the one that closed and must be left alone.
  QPointer<FeedWriter> closingWriter = m_feedWriter.get();
  QTimer::singleShot(0, this, [this, closingWriter] {
    if (m_toggleAction != nullptr && m_toggleAction->isChecked())
      return; // The user re-enabled desktop lyrics while we waited.
    if (m_feedWriter.get() != closingWriter)
      return; // Replaced (or already torn down): the new session owns its lifecycle.
    stopDesktopLyrics();
  });
}

void LxLyricsPlugin::onAppExited(int status, bool closeRequested)
{
  // The child process is gone; FeedWriter has already cleared its own process
  // bookkeeping before emitting this, so every path below only decides what
  // the SESSION does next.
  if (closeRequested) {
    // The user closed the lyric window: onCloseRequested() already unchecked
    // the toggle and scheduled the teardown. The exit is expected, never a
    // crash; the deferred teardown is idempotent when it finds the writer
    // already stopped.
    return;
  }

  if (status != 0) {
    // protocol.md §7: a protocol violation aborts the app with a non-zero
    // exit status. This is NOT a crash to respawn — a respawn would loop on
    // the same malformed input. Log loudly, tear the session down (the
    // bridge and spectrum source die with it), drop the dead child's
    // bookkeeping and end the session: the toggle is unchecked signal-blocked
    // (no synchronous writer teardown from inside the emitting writer) and
    // the remembered state is written here explicitly, because the blocker
    // suppresses toggleDesktopLyrics().
    qWarning() << "[LX Lyrics] lyrics app aborted (protocol error), exit status" << status
               << "; disabling desktop lyrics";
    teardownSession();
    if (m_toggleAction != nullptr) {
      const QSignalBlocker blocker(m_toggleAction);
      m_toggleAction->setChecked(false);
    }
    rememberState(false);
    if (m_feedWriter != nullptr) {
      m_feedWriter->stop();
    }
    return;
  }

  if (m_toggleAction == nullptr || !m_toggleAction->isChecked()) {
    return;
  }
  if (m_feedWriter == nullptr) {
    return;
  }

  // protocol.md §2: the host may respawn the app after an unexpected exit.
  // Guard against a stale timer: the user may toggle off/on during the
  // 1500 ms delay, starting a new child; the old timer must then be dropped.
  QPointer<FeedWriter> exitedWriter = m_feedWriter.get();
  qInfo() << "[LX Lyrics] app exited; respawning in 1500 ms";
  QTimer::singleShot(1500, this, [this, exitedWriter] {
    if (m_feedWriter.get() != exitedWriter) {
      // Replaced while waiting (toggle off/on): drop the stale respawn — the
      // new session owns its own lifecycle.
      return;
    }
    if (m_toggleAction == nullptr || !m_toggleAction->isChecked()) {
      return;
    }
    if (m_feedWriter->isRunning()) {
      return; // A child is already up (restarted by a toggle): nothing to recover.
    }
    // Respawn only fires while the toggle is checked, so the desktop lyrics
    // are wanted; spawn unconditionally (the session objects survived the
    // crash — this is the same guarantee a fresh start gives).
    ensureSessionObjects();
    if (!m_feedWriter->spawn()) {
      qWarning() << "[LX Lyrics] failed to respawn lyrics app; disabling desktop lyrics";
      m_toggleAction->setChecked(false);
    }
  });
}

void LxLyricsPlugin::applyFeedWriterSettings()
{
  if (m_feedWriter == nullptr || m_settingsManager == nullptr) {
    return;
  }
  m_feedWriter->setAppPath(m_settingsManager->value(LxLyrics::appPathKey).toString());
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
  if (m_feedWriter != nullptr) {
    m_feedWriter->sendSetFullscreen(isFullscreen);
  }
}

void LxLyricsPlugin::startHotkey()
{
  if (m_hotkey != nullptr || m_settingsManager == nullptr) {
    return;
  }

  const QString helper = resolveHotkeyHelper();
  if (helper.isEmpty()) {
    qWarning() << "[LX Lyrics] lx-lyrics-hotkey not found; the global shortcut is unavailable";
    return;
  }

  // lx_hotkey_start() forks/execs synchronously, so the encoded strings only
  // need to outlive the call.
  const QByteArray helperPath = QFile::encodeName(helper);
  const QByteArray accelerator =
    m_settingsManager->value(LxLyrics::hotkeyKey).toString().trimmed().toUtf8();
  const lx_hotkey_config_t config = {
    .helper_path = helperPath.constData(),
    .accelerator = accelerator.isEmpty() ? nullptr : accelerator.constData(),
    .on_toggle = onHotkeyToggle,
    .on_status = onHotkeyStatus,
    .userdata = this,
  };
  m_hotkey = lx_hotkey_start(&config);
  if (m_hotkey == nullptr) {
    qWarning() << "[LX Lyrics] cannot spawn" << helper << "; the global shortcut is unavailable";
    return;
  }

  qInfo() << "[LX Lyrics] global-shortcut helper started:" << helper;
}

void LxLyricsPlugin::stopHotkey()
{
  if (m_hotkey == nullptr) {
    return;
  }
  // Stops the reader, closes the pipe, then SIGTERM→SIGKILL and reaps the
  // helper; the handle has no destroy call (the transport's contract).
  lx_hotkey_stop(m_hotkey);
  m_hotkey = nullptr;
}

QString LxLyricsPlugin::resolveHotkeyHelper() const
{
  if (m_settingsManager == nullptr) {
    return {};
  }

  // A configured value is the user's explicit choice: a path, or a bare name
  // looked up on PATH. A value that cannot run is reported as "not found".
  const QString configured =
    m_settingsManager->value(LxLyrics::hotkeyHelperKey).toString().trimmed();
  if (!configured.isEmpty()) {
    const QString resolved = configured.contains(QLatin1Char('/'))
                               ? configured
                               : QStandardPaths::findExecutable(configured);
    return QFileInfo(resolved).isExecutable() ? resolved : QString();
  }

  // Otherwise next to the app (the installer puts both into <prefix>/bin),
  // using the same AppPath resolution the feed writer uses, then PATH.
  QString app = m_settingsManager->value(LxLyrics::appPathKey).toString().trimmed();
  if (app.isEmpty()) {
    app = QStandardPaths::findExecutable(QStringLiteral("lx-lyrics-app"));
  }
  if (app.isEmpty()) {
    app =
      QCoreApplication::applicationDirPath() + QLatin1Char('/') + QStringLiteral("lx-lyrics-app");
  }

  const QString beside =
    QFileInfo(app).absolutePath() + QLatin1Char('/') + QStringLiteral("lx-lyrics-hotkey");
  if (QFileInfo(beside).isExecutable()) {
    return beside;
  }
  return QStandardPaths::findExecutable(QStringLiteral("lx-lyrics-hotkey"));
}

void LxLyricsPlugin::onHotkeyStatus(const char* line, void* userdata)
{
  (void)userdata;
  // Runs on the helper's reader thread; logging is thread-safe and this
  // touches no plugin state. The helper writes each status line once.
  const QString text = QString::fromLocal8Bit(line != nullptr ? line : "");
  if (text.startsWith(QLatin1String("ready "))) {
    qInfo() << "[LX Lyrics] global shortcut bound to" << text.mid(6);
  } else if (text.startsWith(QLatin1String("unavailable "))) {
    qWarning() << "[LX Lyrics] global shortcut unavailable" << text.mid(12);
  } else if (!text.isEmpty()) {
    qInfo() << "[LX Lyrics] hotkey helper:" << text;
  }
}

void LxLyricsPlugin::onHotkeyToggle(void* userdata)
{
  auto* self = static_cast<LxLyricsPlugin*>(userdata);
  if (self == nullptr) {
    return;
  }
  // The helper's reader thread must never touch the action or the session:
  // queue the flip onto the Qt main thread, where a View-menu click runs.
  QMetaObject::invokeMethod(
    self,
    [self] {
      self->toggleFromHotkey();
    },
    Qt::QueuedConnection);
}

void LxLyricsPlugin::toggleFromHotkey()
{
  if (m_toggleAction == nullptr) {
    qWarning() << "[LX Lyrics] global shortcut pressed before the GUI initialised; ignored";
    return;
  }

  // Exactly what clicking View → Desktop Lyrics does: flip the checked action
  // and let its toggled() signal run the normal start/stop path.
  m_toggleAction->setChecked(!m_toggleAction->isChecked());
  qInfo() << "[LX Lyrics] desktop lyrics" << (m_toggleAction->isChecked() ? "enabled" : "disabled")
          << "from the global shortcut";
}
