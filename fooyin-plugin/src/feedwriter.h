/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */
#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantMap>

class QJsonObject;

/// Player-side half of the player feed (docs/protocol.md v2).
///
/// The adapter owns the display app as a DIRECT CHILD (never detached) with
/// three pipes (§2): the app's stdin carries the JSON lines below, its stdout
/// carries the two app→host actions, its stderr is free-form logs. The lyric
/// window therefore lives and dies with the player process.
///
/// Fooyin-header-free on purpose: lyrics-app's `feed` suite compiles this file
/// to assert both directions of the wire (the same pattern the deleted
/// HostServer used).
class FeedWriter : public QObject {
  Q_OBJECT

public:
  static constexpr int kProtocolVersion = 2;

  explicit FeedWriter(QObject* parent = nullptr);
  ~FeedWriter() override;

  /// Empty → resolution at spawn time: `QStandardPaths::findExecutable`
  /// ("lx-lyrics-app" on PATH), then the player's own directory
  /// (QCoreApplication::applicationDirPath()).
  void setAppPath(const QString& path);
  /// Declared in the hello line. The plugin sets it from
  /// EngineController::visualisationService() != nullptr.
  void setSpectrumAvailable(bool available);

  /// Starts `<app> --player-feed`, waits for the child to come up and writes
  /// the hello line. False when the binary is missing or never started.
  ///
  /// Called on the GUI thread, so the wait is bounded and short: the child is
  /// a Qt process that execs immediately, and a launch that has not started
  /// within the start budget has failed. Same for stop() — together they are
  /// the only places the plugin blocks the player's UI thread, for at most
  /// 1.5 s + 1 s + 2 x 250 ms in the worst case (see the timeouts in the .cpp).
  bool spawn();
  /// Ends the session: EOF on the child's stdin (protocol §2 — the app quits
  /// immediately), then terminate/kill if it does not. A stop the plugin asked
  /// for does NOT emit appExited(): the session was torn down deliberately, not
  /// lost.
  void stop();
  [[nodiscard]] bool isRunning() const;
  /// Exit code of the last finished child, as QProcess reports it: the child's
  /// own status on a normal exit, and 0 when it was signalled (QProcess'
  /// contract, not "-1"). The crash mapping the plugin acts on is the
  /// `status` argument of appExited(), where a signalled child is reported
  /// as -1.
  [[nodiscard]] int exitCode() const;

  /// Outbound host→app helpers (protocol §2/§3). Each writes ONE
  /// {"v":2,"action":…} JSON line and flushes it. All are safe no-ops while
  /// the child is not running.
  void sendSetInfo(const QVariantMap& fields);
  void sendSetLyric(const QString& lrc, const QString& tlrc, const QString& rlrc,
                    const QString& lxlrc);
  void sendSetStatus(bool isPlay, qint64 playedTime);
  void sendSetOffset(qint64 tempOffset);
  void sendSetPlaybackRate(double rate);
  void sendSetPlay(qint64 time);
  void sendSetPause();
  void sendSetStop();
  void sendSetFullscreen(bool isFullscreen);
  void sendOpenSettings();
  /// Base64 into a {"v":2,"action":"spectrum"} line. Must be exactly 128 bytes
  /// (protocol §5); anything else is refused loudly and not sent.
  void sendAnalyserData(const QByteArray& data);

signals:
  /// Child up and the hello line written — the plugin re-pushes the fullscreen
  /// state here.
  void appStarted();
  /// The child exited on its own. `status` is the exit code (non-zero =
  /// protocol error, per §2 the app exits non-zero on a violation);
  /// `closeRequested` is true when the user closed the lyric window first.
  void appExited(int status, bool closeRequested);
  /// The child asked for one analyser frame.
  void analyserDataRequested();
  /// The user closed the lyric window (control-bar X or WM close).
  void closeRequested();

private:
  void onReadyReadStandardOutput();
  /// Forwards the app's free-form logs (§2 stderr) into the player's log.
  /// Draining it is also what keeps the child from blocking on a full pipe.
  void onReadyReadStandardError();
  void onFinished(int code, QProcess::ExitStatus status);
  /// Strict parse of one app→host line (protocol §6). Anything that is not one
  /// of the two v2 actions — including non-JSON noise — is a protocol error
  /// from the app: log loudly and end the session.
  void dispatchLine(const QByteArray& line);
  void failProtocol(const QString& reason);
  [[nodiscard]] QString resolveAppPath() const;
  void sendJson(const QString& action, const QJsonObject& fields);

  QProcess* m_process = nullptr;
  QString m_appPath;
  bool m_spectrumAvailable = false;
  bool m_stopping = false;
  bool m_closeRequested = false;
  bool m_protocolFailed = false;
  QByteArray m_stdoutBuffer;
};
