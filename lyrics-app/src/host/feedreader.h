/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include "host/feedtypes.h"

class QIODevice;
class QJsonObject;

// Protocol v2 client for the player feed (docs/protocol.md): a player-side
// adapter spawns this app as a DIRECT CHILD and talks to it over the child's
// stdin (host→app JSON lines) and stdout (app→host JSON lines).
//
// Transport + strict parsing only: complete lines are read from `source`,
// parsed at the boundary into the typed snapshots above, and dispatched as
// signals; the two app→host actions are written as one JSON line each to
// `sink`. Per §7/§6 any deviation — a message before `hello`, a missing or
// unsupported `v`, malformed JSON, an unknown action, a wrong-typed field, a
// line over 1 MiB — is a protocol error: `protocolError()` is emitted and
// nothing is half-applied (main() logs loudly and exits non-zero). EOF on
// `source` means the player is gone: `exited()`.
class FeedReader : public QObject {
  Q_OBJECT

public:
  static constexpr int kProtocolVersion = 2;

  // `source` = the app's stdin, `sink` = the app's stdout.
  FeedReader(QIODevice* source, QIODevice* sink, QObject* parent = nullptr);

  // The host declared analyser availability in its hello line.
  [[nodiscard]] bool spectrumSupported() const;
  // Ask for one spectrum frame. No-op until hello declared spectrum:true (§5:
  // the app only requests frames when the host has an analyser).
  void requestAnalyserData();
  // §4: the user closed the lyric window; the player ends its session and must
  // not treat the exit as a crash.
  void sendCloseRequested();

signals:
  // set_info, with the lyrics already resolved: the host's non-empty `lrc`
  // wins, otherwise the file at `path` was read (sidecar first, then embedded).
  void infoReceived(const TrackSnapshot& info);
  void lyricReceived(const LyricSnapshot& lyric);
  void statusReceived(const PlaybackSnapshot& status);
  void offsetReceived(qint64 tempOffset);
  void playbackRateReceived(double rate);
  void playReceived(qint64 timeMs);
  void pauseReceived();
  void stopReceived();
  void openSettingsRequested();
  void fullscreenReceived(bool isFullscreen);
  // Exactly 128 bytes of log-scaled spectrum magnitudes (§5); other sizes are
  // dropped loudly, never fatal.
  void analyserDataReceived(const QByteArray& data);
  // The host closed the pipe (stdin EOF): quit immediately, no animation.
  void exited();
  // Strict parse failure (§7): the session is over, the app must not continue.
  void protocolError(const QString& reason);

private:
  void readAvailable();
  void dispatchLine(const QByteArray& line);
  void failProtocol(const QString& reason);
  void sendJson(const QString& action, const QJsonObject& fields);

  QIODevice* m_source = nullptr;
  QIODevice* m_sink = nullptr;
  QByteArray m_buffer;
  bool m_helloSeen = false;
  bool m_spectrum = false;
  bool m_failed = false;
  bool m_exited = false;
  // Latched when the sink refuses a line (the host stopped reading its stdout,
  // or its reader is gone): the first failure warns, later writes stay silent
  // instead of repeating the warning once per spectrum frame.
  bool m_sinkBroken = false;
};
