/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include <utils/settings/settingsmanager.h>
#include <utils/settings/settingspage.h>

#include <QObject>
#include <QString>
#include <QWidget>

#include <functional>

class QCheckBox;
class QLineEdit;
class QPushButton;

namespace LxLyrics {

// Registered settings keys (Fooyin::SettingsManager::createSetting).
// User-editable, read/written by the page: AppPath (empty means the feed
// writer auto-detects via PATH / the app's own directory) and RememberState
// (restore the last desktop-lyrics state across sessions). Enabled is the
// plugin-written remembered state — never shown on the page.
inline const QString appPathKey = QStringLiteral("LxLyrics/AppPath");
inline const QString rememberStateKey = QStringLiteral("LxLyrics/RememberState");
inline const QString enabledKey = QStringLiteral("LxLyrics/Enabled");

} // namespace LxLyrics

/// The settings page widget: app-path edit + remember-state checkbox + a button
/// that opens the running lyrics app's own config dialog. Pure form read/write
/// over the SettingsManager; the plugin owns acting on the values (app path
/// push on spawn, state restore/remember).
class LxLyricsSettingsPageWidget : public Fooyin::SettingsPageWidget {
  Q_OBJECT

public:
  explicit LxLyricsSettingsPageWidget(Fooyin::SettingsManager* settings, QWidget* parent = nullptr);

  void load() override;
  void apply() override;
  void reset() override;

  // Not a setting: the "Open lyrics settings" button just asks the running
  // lyrics app to open its own configuration dialog (protocol.md §5
  // open_settings). The callback is injected by the plugin; the click is a
  // no-op while it is unset or the app is not running.
  void setOpenSettingsCallback(std::function<void()> cb);

private:
  Fooyin::SettingsManager* m_settings;
  QLineEdit* m_appPathEdit;
  QCheckBox* m_rememberStateCheck;
  QPushButton* m_openSettingsButton;
  std::function<void()> m_openSettingsCallback;
};

/// Registered under the "Lyrics" category; constructing it with the settings
/// dialog controller is what registers it (SettingsPage ctor calls addPage).
/// The widget is created lazily by the dialog, so the open-settings callback
/// is stored here and forwarded in the WidgetCreator lambda.
class LxLyricsSettingsPage : public Fooyin::SettingsPage {
  Q_OBJECT

public:
  explicit LxLyricsSettingsPage(Fooyin::SettingsManager* settings, QObject* parent = nullptr);

  /// Stored on the page (the widget may not exist until the dialog opens)
  /// and forwarded to the widget when the WidgetCreator runs.
  void setOpenSettingsCallback(std::function<void()> cb);

private:
  std::function<void()> m_openSettingsCallback;
};
