/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include <gui/plugins/pluginsettingsprovider.h>
#include <utils/settings/settingsmanager.h>

#include <QDialog>
#include <QString>

#include <functional>

class QCheckBox;
class QLineEdit;
class QPushButton;

namespace LxLyrics {

// Registered settings keys (Fooyin::SettingsManager::createSetting).
// User-editable, read/written by the settings dialog: AppPath (empty means the
// feed writer auto-detects via PATH / the app's own directory) and
// RememberState (restore the last desktop-lyrics state across sessions).
// Enabled is the plugin-written remembered state — never shown in the dialog.
inline const QString appPathKey = QStringLiteral("LxLyrics/AppPath");
inline const QString rememberStateKey = QStringLiteral("LxLyrics/RememberState");
inline const QString enabledKey = QStringLiteral("LxLyrics/Enabled");

} // namespace LxLyrics

/// The plugin's configuration dialog, opened from the Plugins page's
/// "Configure" button (Fooyin's dedicated plugin-configuration surface).
/// App-path edit + remember-state checkbox + a button that opens the running
/// lyrics app's own config dialog. Pure form read/write over the
/// SettingsManager; the plugin owns acting on the values (app path push on
/// spawn, state restore/remember). Cancel discards the edits; the initial
/// values are read at construction, so a single-instance dialog never shows
/// stale state.
class LxLyricsSettingsDialog : public QDialog {
  Q_OBJECT

public:
  explicit LxLyricsSettingsDialog(Fooyin::SettingsManager* settings,
                                  std::function<void()> openSettings, QWidget* parent = nullptr);

  void accept() override;

private:
  Fooyin::SettingsManager* m_settings;
  QLineEdit* m_appPathEdit;
  QCheckBox* m_rememberStateCheck;
  QPushButton* m_openSettingsButton;
};

/// Provides the dialog to Fooyin. The host creates the provider once at
/// startup (GuiApplication::initialise) and calls showSettings() from the
/// Plugins page, which builds the dialog on first use and re-raises the open
/// one afterwards — hence the dialog is created lazily here, never in the ctor.
class LxLyricsPluginSettingsProvider : public Fooyin::PluginSettingsProvider {
public:
  LxLyricsPluginSettingsProvider(Fooyin::SettingsManager* settings,
                                 std::function<void()> openSettings);

protected:
  QDialog* createSettings(QWidget* parent) override;

private:
  Fooyin::SettingsManager* m_settings;
  std::function<void()> m_openSettings;
};
