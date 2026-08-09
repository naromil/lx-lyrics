/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include <QObject>
#include <QString>

class QUrl;

class AppSpawner : public QObject {
  Q_OBJECT

public:
  explicit AppSpawner(QObject* parent = nullptr);

  /// Explicit path to the lyrics-app binary. Empty (the default) means
  /// auto-detect: search order is (1) this set path, (2) "lyrics-app" on
  /// PATH via QStandardPaths::findExecutable, (3) fooyin's bin directory
  /// (QCoreApplication::applicationDirPath() + "/lyrics-app").
  void setAppPath(const QString& appPath);
  [[nodiscard]] const QString& appPath() const;

  /// Whether the init-time auto-spawn may launch the app. Default false,
  /// matching the LxLyrics/AutoSpawn setting key. Manual (forced) spawns
  /// ignore this flag.
  void setAutoSpawn(bool autoSpawn);
  [[nodiscard]] bool autoSpawn() const;

  /// Launches the app with --ws=<url> --exit-on-disconnect. Returns false
  /// (without launching) when an instance is already running or the binary
  /// cannot be located/started. AutoSpawn gates only the init-time
  /// auto-spawn; manual (menu toggle) spawn always allowed.
  /// With force=false, auto-spawn off also blocks the launch.
  bool spawn(const QUrl& wsUrl, bool force = false);

  /// True between a successful spawn() and the next stop()/failed relaunch.
  /// The app is detached, so its exit is not observed directly; stop()
  /// resets the flag without touching the running process.
  [[nodiscard]] bool isRunning() const;

  /// Resets the running flag. Deliberately does NOT terminate the detached
  /// app: it exits on its own when the host socket closes
  /// (--exit-on-disconnect).
  void stop();

private:
  [[nodiscard]] QString resolveAppPath() const;

  QString m_appPath;
  bool m_autoSpawn = false;
  bool m_running = false;
};
