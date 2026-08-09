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
  /// Push the plugin-side settings (app path / auto-spawn) into the spawner
  /// before every launch; called again on setting change via subscribe().
  void applySpawnerSettings();
  [[nodiscard]] QUrl serverWsUrl() const;

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
};
