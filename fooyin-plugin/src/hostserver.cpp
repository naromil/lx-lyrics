/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "hostserver.h"

#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace {
/// Close code used for all protocol errors (valid application range 4000-4999).
QWebSocketProtocol::CloseCode protocolErrorCloseCode()
{
  return static_cast<QWebSocketProtocol::CloseCode>(4000);
}
} // namespace

HostServer::HostServer(QObject* parent)
  : QObject(parent)
  , m_server(
      new QWebSocketServer(QStringLiteral("lx-lyrics"), QWebSocketServer::NonSecureMode, this))
{
  if (!m_server->listen(QHostAddress::LocalHost, 0)) {
    qWarning() << "[LX Lyrics] HostServer failed to listen:" << m_server->errorString();
    return;
  }

  connect(m_server, &QWebSocketServer::newConnection, this, &HostServer::onNewConnection);

  qInfo() << "[LX Lyrics] HostServer listening on ws://127.0.0.1:" << m_server->serverPort();
}

HostServer::~HostServer()
{
  if (m_client != nullptr) {
    // Closing the socket makes an app spawned with --exit-on-disconnect quit.
    m_client->close();
    m_client->deleteLater();
  }
  m_server->close();
}

quint16 HostServer::serverPort() const
{
  return m_server->serverPort();
}

bool HostServer::isListening() const
{
  return m_server->isListening();
}

void HostServer::sendSetInfo(const QVariantMap& fields)
{
  QJsonObject object{{QStringLiteral("v"), 1},
                     {QStringLiteral("action"), QStringLiteral("set_info")}};
  for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
    object.insert(it.key(), QJsonValue::fromVariant(it.value()));
  }
  sendJson(QStringLiteral("set_info"), object);
}

void HostServer::sendSetLyric(const QString& lrc, const QString& tlrc, const QString& rlrc,
                              const QString& lxlrc)
{
  sendJson(QStringLiteral("set_lyric"), {{QStringLiteral("lrc"), lrc},
                                         {QStringLiteral("tlrc"), tlrc},
                                         {QStringLiteral("rlrc"), rlrc},
                                         {QStringLiteral("lxlrc"), lxlrc}});
}

void HostServer::sendSetStatus(bool isPlay, int line, qint64 playedTime)
{
  sendJson(QStringLiteral("set_status"), {{QStringLiteral("isPlay"), isPlay},
                                          {QStringLiteral("line"), line},
                                          {QStringLiteral("played_time"), playedTime}});
}

void HostServer::sendSetOffset(qint64 tempOffset)
{
  sendJson(QStringLiteral("set_offset"), {{QStringLiteral("tempOffset"), tempOffset}});
}

void HostServer::sendSetPlaybackRate(double rate)
{
  sendJson(QStringLiteral("set_playbackRate"), {{QStringLiteral("rate"), rate}});
}

void HostServer::sendSetFullscreen(bool isFullscreen)
{
  sendJson(QStringLiteral("set_fullscreen"), {{QStringLiteral("isFullscreen"), isFullscreen}});
}

void HostServer::sendSetPlay(qint64 time)
{
  sendJson(QStringLiteral("set_play"), {{QStringLiteral("time"), time}});
}

void HostServer::sendSetPause()
{
  sendJson(QStringLiteral("set_pause"), {});
}

void HostServer::sendSetStop()
{
  sendJson(QStringLiteral("set_stop"), {});
}

void HostServer::sendOpenSettings()
{
  sendJson(QStringLiteral("open_settings"), {});
}

void HostServer::sendAnalyserData(const QByteArray& data)
{
  Q_ASSERT(data.size() == 128);
  if (data.size() != 128) {
    qWarning() << "[LX Lyrics] refusing to send analyser frame:" << data.size()
               << "bytes (protocol requires exactly 128)";
    return;
  }
  if (m_client == nullptr) {
    return;
  }
  m_client->sendBinaryMessage(data);
}

void HostServer::onNewConnection()
{
  QWebSocket* socket = m_server->nextPendingConnection();
  if (socket == nullptr) {
    return;
  }

  if (m_client != nullptr) {
    // protocol.md §8: exactly one client.
    qWarning() << "[LX Lyrics] rejecting second client (only one allowed)";
    socket->close(protocolErrorCloseCode(), QStringLiteral("only one client allowed"));
    socket->deleteLater();
    return;
  }

  m_client = socket;
  connect(socket, &QWebSocket::textMessageReceived, this, &HostServer::onTextMessageReceived);
  connect(socket, &QWebSocket::binaryMessageReceived, this, &HostServer::onBinaryMessageReceived);
  connect(socket, &QWebSocket::disconnected, this, &HostServer::onClientDisconnected);

  qInfo() << "[LX Lyrics] client connected";
  emit clientConnected();
}

void HostServer::onTextMessageReceived(const QString& message)
{
  auto* client = qobject_cast<QWebSocket*>(sender());
  if (client == nullptr || client != m_client) {
    return;
  }

  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    closeForProtocolError(QStringLiteral("malformed JSON"));
    return;
  }

  const QJsonObject object = document.object();

  // protocol.md §9: every message carries the protocol version, integer 1.
  const QJsonValue version = object.value(QStringLiteral("v"));
  if (!version.isDouble() || version.toDouble() != 1.0) {
    closeForProtocolError(QStringLiteral("unsupported protocol version"));
    return;
  }

  const QJsonValue action = object.value(QStringLiteral("action"));
  if (!action.isString()) {
    closeForProtocolError(QStringLiteral("missing or invalid action"));
    return;
  }

  const QString actionName = action.toString();
  if (actionName == QLatin1String("get_info")) {
    emit requestInfo();
  } else if (actionName == QLatin1String("get_status")) {
    emit requestStatus();
  } else if (actionName == QLatin1String("get_analyser_data_array")) {
    emit requestAnalyserData();
  } else if (actionName == QLatin1String("close_requested")) {
    // protocol.md §4: the user intentionally closed the lyric window. The
    // plugin ends its session without respawning (the following disconnect
    // must not enter the crash-recovery path). No payload to validate.
    emit closeRequested();
  } else {
    closeForProtocolError(QStringLiteral("unknown action: %1").arg(actionName));
  }
}

void HostServer::onBinaryMessageReceived(const QByteArray& message)
{
  Q_UNUSED(message)
  auto* client = qobject_cast<QWebSocket*>(sender());
  if (client == nullptr || client != m_client) {
    return;
  }
  // protocol.md §5: the only binary frame type is host->app; an app->host
  // binary frame is outside the contract, so fail loud per §7.
  closeForProtocolError(QStringLiteral("unexpected binary frame"));
}

void HostServer::onClientDisconnected()
{
  auto* client = qobject_cast<QWebSocket*>(sender());
  if (client == nullptr || client != m_client) {
    return;
  }

  m_client = nullptr;
  client->deleteLater();

  qInfo() << "[LX Lyrics] client disconnected";
  emit clientDisconnected();
}

void HostServer::closeForProtocolError(const QString& reason)
{
  if (m_client == nullptr) {
    return;
  }

  qWarning() << "[LX Lyrics] protocol error, closing client:" << reason;

  QWebSocket* client = m_client;
  m_client = nullptr;
  client->close(protocolErrorCloseCode(), reason);
  client->deleteLater();

  // Dedicated signal, deliberately NOT clientDisconnected: the normal
  // disconnect event is a session-crash/reconnect trigger for the plugin,
  // while a protocol violation is a malformed-client event — the plugin must
  // reset bookkeeping without scheduling a respawn. Clearing m_client first
  // (above) keeps the socket's own disconnected signal from reaching
  // onClientDisconnected, exactly as before.
  emit protocolErrorClosed();
}

void HostServer::sendJson(const QString& action, const QJsonObject& fields)
{
  if (m_client == nullptr) {
    return;
  }

  QJsonObject message{{QStringLiteral("v"), 1}, {QStringLiteral("action"), action}};
  for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
    message.insert(it.key(), it.value());
  }

  m_client->sendTextMessage(
    QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}
