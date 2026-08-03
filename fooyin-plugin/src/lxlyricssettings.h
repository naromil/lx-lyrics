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

class QCheckBox;
class QLineEdit;

namespace LxLyrics {

// Registered settings keys (Fooyin::SettingsManager::createSetting). Shared by
// the page (read/write via value/set) and the plugin (subscribe + apply on
// spawn). Empty app path means AppSpawner auto-detects (PATH / bin dir).
inline const QString appPathKey   = QStringLiteral("LxLyrics/AppPath");
inline const QString autoSpawnKey = QStringLiteral("LxLyrics/AutoSpawn");

} // namespace LxLyrics

/// The settings page widget: app-path edit + auto-spawn checkbox. Pure form
/// read/write over the SettingsManager; the plugin owns applying the values to
/// the AppSpawner (via setting subscriptions).
class LxLyricsSettingsPageWidget : public Fooyin::SettingsPageWidget
{
    Q_OBJECT

public:
    explicit LxLyricsSettingsPageWidget(Fooyin::SettingsManager* settings, QWidget* parent = nullptr);

    void load() override;
    void apply() override;
    void reset() override;

private:
    Fooyin::SettingsManager* m_settings;
    QLineEdit* m_appPathEdit;
    QCheckBox* m_autoSpawnCheck;
};

/// Registered under the "Lyrics" category; constructing it with the settings
/// dialog controller is what registers it (SettingsPage ctor calls addPage).
class LxLyricsSettingsPage : public Fooyin::SettingsPage
{
    Q_OBJECT

public:
    explicit LxLyricsSettingsPage(Fooyin::SettingsManager* settings, QObject* parent = nullptr);
};
