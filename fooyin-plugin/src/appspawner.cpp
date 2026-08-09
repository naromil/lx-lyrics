/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "appspawner.h"

#include <QCoreApplication>
#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

AppSpawner::AppSpawner(QObject* parent)
  : QObject(parent)
{
}

void AppSpawner::setAppPath(const QString& appPath)
{
  m_appPath = appPath;
}

const QString& AppSpawner::appPath() const
{
  return m_appPath;
}

void AppSpawner::setAutoSpawn(bool autoSpawn)
{
  m_autoSpawn = autoSpawn;
}

bool AppSpawner::autoSpawn() const
{
  return m_autoSpawn;
}

bool AppSpawner::spawn(const QUrl& wsUrl, bool force)
{
  // AutoSpawn gates only the init-time auto-spawn; manual (menu toggle)
  // spawn always allowed.
  if (!m_autoSpawn && !force) {
    return false;
  }
  if (m_running) {
    qWarning() << "[LX Lyrics] lyrics-app already spawned; skipping duplicate launch";
    return false;
  }
  if (!wsUrl.isValid()) {
    qWarning() << "[LX Lyrics] refusing to spawn lyrics-app with invalid ws url:" << wsUrl;
    return false;
  }

  const QString program = resolveAppPath();
  if (program.isEmpty()) {
    qWarning() << "[LX Lyrics] lx-lyrics-app binary not found (PATH / fooyin bin dir)";
    return false;
  }

  // protocol.md §8: argv array, no shell interpolation.
  const QStringList arguments{
    QStringLiteral("--ws=%1").arg(wsUrl.toString()),
    QStringLiteral("--exit-on-disconnect"),
  };

  qint64 pid = 0;
  if (!QProcess::startDetached(program, arguments, QString(), &pid)) {
    qWarning() << "[LX Lyrics] failed to start lyrics-app:" << program;
    return false;
  }

  qInfo() << "[LX Lyrics] spawned lyrics-app (pid" << pid << "):" << program
          << arguments.join(QLatin1Char(' '));
  m_running = true;
  return true;
}

bool AppSpawner::isRunning() const
{
  return m_running;
}

void AppSpawner::stop()
{
  // The app is detached; it exits on its own when the host socket closes
  // (--exit-on-disconnect). We only reset our bookkeeping so a later spawn()
  // (e.g. the plugin's respawn after a disconnect) is allowed again.
  m_running = false;
}

QString AppSpawner::resolveAppPath() const
{
  if (!m_appPath.isEmpty()) {
    return m_appPath;
  }

  QString onPath = QStandardPaths::findExecutable(QStringLiteral("lx-lyrics-app"));
  if (!onPath.isEmpty()) {
    return onPath;
  }

  return QCoreApplication::applicationDirPath() + QLatin1Char('/') +
         QStringLiteral("lx-lyrics-app");
}
