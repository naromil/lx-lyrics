/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QWebSocket;
class QWebSocketServer;

class HostServer : public QObject {
  Q_OBJECT

public:
  explicit HostServer(QObject* parent = nullptr);
  ~HostServer() override;

  /// Port the server bound to after listen(); 0 when not listening.
  [[nodiscard]] quint16 serverPort() const;
  [[nodiscard]] bool isListening() const;

  /// Outbound host->app helpers (protocol.md §5). Each builds
  /// {"v":1,"action":...} and sends a JSON text frame. All are safe no-ops
  /// when no client is connected.
  void sendSetInfo(const QVariantMap& fields);
  void sendSetLyric(const QString& lrc, const QString& tlrc, const QString& rlrc,
                    const QString& lxlrc);
  void sendSetStatus(bool isPlay, int line, qint64 playedTime);
  void sendSetOffset(qint64 tempOffset);
  void sendSetPlaybackRate(double rate);
  void sendSetPlay(qint64 time);
  void sendSetPause();
  void sendSetStop();
  /// Requests the display app to open its configuration dialog
  /// ({"v":1,"action":"open_settings"}); no payload fields.
  void sendOpenSettings();
  /// Binary frame, must be exactly 128 bytes per protocol.md §5.
  void sendAnalyserData(const QByteArray& data);

signals:
  /// The single accepted client connected.
  void clientConnected();
  /// The accepted client is gone (closed or errored). The plugin may respawn
  /// the display app. Protocol-error closes do NOT emit this.
  void clientDisconnected();
  /// App requested {"v":1,"action":"get_info"}.
  void requestInfo();
  /// App requested {"v":1,"action":"get_status"}.
  void requestStatus();
  /// App requested {"v":1,"action":"get_analyser_data_array"}.
  void requestAnalyserData();

private:
  void onNewConnection();
  void onTextMessageReceived(const QString& message);
  void onBinaryMessageReceived(const QByteArray& message);
  void onClientDisconnected();
  /// Logs loudly, closes the offending client and forgets it. Per protocol.md
  /// §7 the host never half-renders partial data.
  void closeForProtocolError(const QString& reason);
  void sendJson(const QString& action, const QJsonObject& fields);

  QWebSocketServer* m_server = nullptr;
  QWebSocket* m_client = nullptr;
};
