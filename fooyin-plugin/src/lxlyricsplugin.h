/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include "appspawner.h"
#include "hostserver.h"
#include "playerbridge.h"
#include "spectrumsource.h"

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

#include <QObject>

#include <memory>
#include <optional>

class LxLyricsSettingsPage;
class QAction;
class QUrl;

namespace Fooyin {
class ActionManager;
class EngineController;
class PlayerController;
class SettingsManager;
} // namespace Fooyin

class LxLyricsPlugin : public QObject,
                       public Fooyin::Plugin,
                       public Fooyin::CorePlugin,
                       public Fooyin::GuiPlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "org.fooyin.fooyin.plugin/1.0" FILE "lxlyrics.json")
  Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin)

public:
  void initialise(const Fooyin::CorePluginContext& context) override;
  void initialise(const Fooyin::GuiPluginContext& context) override;
  void shutdown() override;

private:
  void toggleDesktopLyrics(bool checked);
  void startDesktopLyrics();
  void stopDesktopLyrics();
  void onClientDisconnected();
  /// The app reported the user intentionally closed the lyric window
  /// (HostServer::closeRequested, protocol.md §4): end the desktop-lyrics
  /// session like a toggle-off so the disconnect never enters the
  /// crash-recovery respawn path. Unchecks the toggle IMMEDIATELY
  /// (signal-blocked, so no synchronous teardown) and defers the actual
  /// server teardown one event-loop turn — this slot runs inside the
  /// emitting HostServer's message handler, which must survive its own
  /// signal. Idempotent; only ever invoked with a connected client.
  void onCloseRequested();
  /// A connected client was closed for a protocol violation
  /// (HostServer::protocolErrorClosed): clear stale spawner bookkeeping
  /// only — no respawn, no toggle change, server keeps listening.
  void onProtocolErrorClosed();
  /// Push the plugin-side settings (app path / auto-spawn) into the spawner
  /// before every launch; called again on setting change via subscribe().
  void applySpawnerSettings();
  [[nodiscard]] QUrl serverWsUrl() const;
  /// Recomputes whether any Fooyin window is fullscreen and pushes it to the
  /// app as set_fullscreen (protocol.md §5) when the state changed, or
  /// whenever `force` is set (a freshly connected client must receive the
  /// current state even if it has not changed).
  void updateFullscreen(bool force = false);
  /// Wires windowStateChanged on EVERY window in QGuiApplication::allWindows()
  /// (Qt::UniqueConnection + this as context: destroyed windows auto-clean,
  /// repeated connects dedupe), plus a visibleChanged re-scan hook and a
  /// destroyed re-check, then pushes the current fullscreen state. Called at
  /// startup and from focusWindowChanged; see GuiPlugin::initialise.
  void watchAllWindows();
  // The three slots below are connected from watchAllWindows() as
  // member-function pointers (not lambdas): Qt::UniqueConnection with a
  // functor slot asserts in Debug builds (Qt 6.11 qobject.h).
  /// QWindow::windowStateChanged slot: re-push the fullscreen state.
  void onWindowStateChanged();
  /// QWindow::visibleChanged slot: a newly visible window may not have been
  /// wired yet, so re-scan.
  void onWindowVisibleChanged();
  /// QObject::destroyed slot: the removed window may have been the fullscreen
  /// one, so re-check.
  void onWindowDestroyed();

  // Owned by later tasks (PlayerBridge, lyric sources, settings page).
  // Stored at initialise time; intentionally minimal now.
  Fooyin::PlayerController* m_playerController = nullptr;
  Fooyin::SettingsManager* m_settingsManager = nullptr;
  Fooyin::EngineController* m_engineController = nullptr;
  Fooyin::ActionManager* m_actionManager = nullptr;

  QAction* m_toggleAction = nullptr;
  // Parented to the plugin (QObject parent chain owns it); the member keeps
  // the registration alive for the SettingsManager's dialog.
  LxLyricsSettingsPage* m_settingsPage = nullptr;
  std::unique_ptr<HostServer> m_hostServer;
  std::unique_ptr<PlayerBridge> m_playerBridge;
  std::unique_ptr<SpectrumSource> m_spectrumSource;
  std::unique_ptr<AppSpawner> m_appSpawner;

  // Fullscreen watcher (set_fullscreen): the last fullscreen state pushed to
  // the app; only a CHANGE is reported (except on client connect, where the
  // current state is forced). The per-window observation lives in
  // watchAllWindows(), driven from GuiPlugin::initialise.
  std::optional<bool> m_lastFullscreenSent;
};
