/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "bridge/wsclient.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace {

constexpr int kProtocolVersion = 1;
constexpr int kAnalyserFrameBytes = 128;

// Parses a JSON text frame into a trusted, versioned object plus its action.
// Returns false (with `error` set) for any frame that is not a valid protocol
// message: malformed JSON, a missing or unsupported `v`, or a missing action.
bool parseProtocolMessage(const QString& frame, QJsonObject* obj, QString* action, QString* error)
{
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(frame.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    *error = QStringLiteral("malformed JSON: %1").arg(parseError.errorString());
    return false;
  }

  *obj = doc.object();

  const QJsonValue version = obj->value(QStringLiteral("v"));
  if (!version.isDouble() || version.toInt() != kProtocolVersion) {
    *error = QStringLiteral("unsupported protocol version (v=%1)")
               .arg(version.isDouble() ? QString::number(version.toDouble())
                                       : QStringLiteral("<missing>"));
    return false;
  }

  const QJsonValue actionValue = obj->value(QStringLiteral("action"));
  if (!actionValue.isString() || actionValue.toString().isEmpty()) {
    *error = QStringLiteral("missing action");
    return false;
  }
  *action = actionValue.toString();
  return true;
}

// String payload fields accept a JSON string or JSON null (the latter becomes
// the empty string, per protocol §6 "May be the empty string when absent").
// Any other type is a protocol violation.
bool parseStringField(const QJsonObject& obj, const QString& key, QString* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || !(value.isString() || value.isNull())) {
    *error = QStringLiteral("'%1' is missing or not a string").arg(key);
    return false;
  }
  *out = value.isNull() ? QString() : value.toString();
  return true;
}

bool parseInt64Field(const QJsonObject& obj, const QString& key, qint64* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || !value.isDouble()) {
    *error = QStringLiteral("'%1' is missing or not a number").arg(key);
    return false;
  }
  *out = value.toInteger();
  return true;
}

bool parseBoolField(const QJsonObject& obj, const QString& key, bool* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || !value.isBool()) {
    *error = QStringLiteral("'%1' is missing or not a boolean").arg(key);
    return false;
  }
  *out = value.toBool();
  return true;
}

bool parseDoubleField(const QJsonObject& obj, const QString& key, double* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || !value.isDouble()) {
    *error = QStringLiteral("'%1' is missing or not a number").arg(key);
    return false;
  }
  *out = value.toDouble();
  return true;
}

// Field chains shared by several snapshots (§5): the lyric text bundle
// (lrc/tlrc/rlrc/lxlrc) appears in both the track and lyric snapshots, and the
// playback state (isPlay/line/played_time) in both the track and playback
// snapshots. Parsed once here so the snapshot parsers below stay in sync.
bool parseLyricFields(const QJsonObject& obj, QString* lrc, QString* tlrc, QString* rlrc,
                      QString* lxlrc, QString* error)
{
  return parseStringField(obj, QStringLiteral("lrc"), lrc, error) &&
         parseStringField(obj, QStringLiteral("tlrc"), tlrc, error) &&
         parseStringField(obj, QStringLiteral("rlrc"), rlrc, error) &&
         parseStringField(obj, QStringLiteral("lxlrc"), lxlrc, error);
}

bool parsePlaybackFields(const QJsonObject& obj, bool* isPlay, qint64* line, qint64* playedTimeMs,
                         QString* error)
{
  return parseBoolField(obj, QStringLiteral("isPlay"), isPlay, error) &&
         parseInt64Field(obj, QStringLiteral("line"), line, error) &&
         parseInt64Field(obj, QStringLiteral("played_time"), playedTimeMs, error);
}

bool parseTrackSnapshot(const QJsonObject& obj, TrackSnapshot* out, QString* error)
{
  return parseStringField(obj, QStringLiteral("id"), &out->id, error) &&
         parseStringField(obj, QStringLiteral("singer"), &out->singer, error) &&
         parseStringField(obj, QStringLiteral("name"), &out->name, error) &&
         parseStringField(obj, QStringLiteral("album"), &out->album, error) &&
         parseLyricFields(obj, &out->lrc, &out->tlrc, &out->rlrc, &out->lxlrc, error) &&
         parsePlaybackFields(obj, &out->isPlay, &out->line, &out->playedTimeMs, error);
}

bool parseLyricSnapshot(const QJsonObject& obj, LyricSnapshot* out, QString* error)
{
  return parseLyricFields(obj, &out->lrc, &out->tlrc, &out->rlrc, &out->lxlrc, error);
}

bool parsePlaybackSnapshot(const QJsonObject& obj, PlaybackSnapshot* out, QString* error)
{
  return parsePlaybackFields(obj, &out->isPlay, &out->line, &out->playedTimeMs, error);
}

} // namespace

WsClient::WsClient(int reconnectIntervalMs, QObject* parent)
  : QObject(parent)
  , m_reconnectIntervalMs(reconnectIntervalMs)
{
  m_reconnectTimer.setSingleShot(true);
  m_reconnectTimer.setInterval(m_reconnectIntervalMs);
  connect(&m_reconnectTimer, &QTimer::timeout, this, &WsClient::retryConnect);

  connect(&m_socket, &QWebSocket::connected, this, [this] {
    m_reconnectTimer.stop();
    emit connected();
  });
  connect(&m_socket, &QWebSocket::disconnected, this, &WsClient::disconnected);
  connect(&m_socket, &QWebSocket::stateChanged, this, &WsClient::onSocketStateChanged);
  connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
    qWarning() << "WsClient: socket error" << error << m_socket.errorString();
  });
  connect(&m_socket, &QWebSocket::textMessageReceived, this, &WsClient::dispatchIncomingText);
  connect(&m_socket, &QWebSocket::binaryMessageReceived, this, &WsClient::dispatchIncomingBinary);
}

void WsClient::connectToHost(const QUrl& url)
{
  m_url = url;
  m_connectionExpected = true;
  m_reconnectTimer.stop();
  if (m_socket.state() != QAbstractSocket::UnconnectedState)
    m_socket.abort();
  openSocket();
}

void WsClient::disconnectFromHost()
{
  m_connectionExpected = false;
  m_reconnectTimer.stop();
  m_socket.close();
}

bool WsClient::isConnected() const
{
  return m_socket.state() == QAbstractSocket::ConnectedState;
}

void WsClient::sendGetInfo()
{
  sendRequest(QStringLiteral("get_info"));
}

void WsClient::sendGetStatus()
{
  sendRequest(QStringLiteral("get_status"));
}

void WsClient::sendGetAnalyserData()
{
  sendRequest(QStringLiteral("get_analyser_data_array"));
}

void WsClient::sendCloseRequested()
{
  // §4 close_requested: user-initiated close of the lyric window. The host
  // stops its session without respawning; dropped when disconnected (the
  // host may already be gone — that is not an error).
  sendRequest(QStringLiteral("close_requested"));
}

void WsClient::openSocket()
{
  if (!m_url.isValid() || m_url.scheme() != QStringLiteral("ws")) {
    qWarning() << "WsClient: refusing invalid WebSocket URL" << m_url;
    m_connectionExpected = false;
    return;
  }
  m_socket.open(m_url);
}

void WsClient::retryConnect()
{
  if (!m_connectionExpected)
    return; // disconnectFromHost() won the race.
  if (m_socket.state() != QAbstractSocket::UnconnectedState) {
    // Previous attempt still tearing down; retry after another interval.
    m_reconnectTimer.start();
    return;
  }
  openSocket();
}

void WsClient::onSocketStateChanged(QAbstractSocket::SocketState state)
{
  if (state == QAbstractSocket::UnconnectedState && m_connectionExpected)
    m_reconnectTimer.start();
}

void WsClient::sendRequest(const QString& action)
{
  if (!isConnected())
    return; // No host right now; drop like the reference sendDesktopLyricInfo.
  const QJsonObject request{
    {QStringLiteral("v"), kProtocolVersion},
    {QStringLiteral("action"), action},
  };
  m_socket.sendTextMessage(
    QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
}

void WsClient::dispatchIncomingText(const QString& frame)
{
  QString error;
  QJsonObject obj;
  QString action;
  if (!parseProtocolMessage(frame, &obj, &action, &error)) {
    failProtocol(error);
    return;
  }

  if (action == QStringLiteral("set_info")) {
    TrackSnapshot snapshot;
    if (!parseTrackSnapshot(obj, &snapshot, &error)) {
      failProtocol(error);
      return;
    }
    emit infoReceived(snapshot);
  } else if (action == QStringLiteral("set_lyric")) {
    LyricSnapshot snapshot;
    if (!parseLyricSnapshot(obj, &snapshot, &error)) {
      failProtocol(error);
      return;
    }
    emit lyricReceived(snapshot);
  } else if (action == QStringLiteral("set_status")) {
    PlaybackSnapshot snapshot;
    if (!parsePlaybackSnapshot(obj, &snapshot, &error)) {
      failProtocol(error);
      return;
    }
    emit statusReceived(snapshot);
  } else if (action == QStringLiteral("set_offset")) {
    qint64 tempOffset = 0;
    if (!parseInt64Field(obj, QStringLiteral("tempOffset"), &tempOffset, &error)) {
      failProtocol(error);
      return;
    }
    emit offsetReceived(tempOffset);
  } else if (action == QStringLiteral("set_playbackRate")) {
    double rate = 1.0;
    if (!parseDoubleField(obj, QStringLiteral("rate"), &rate, &error)) {
      failProtocol(error);
      return;
    }
    emit playbackRateReceived(rate);
  } else if (action == QStringLiteral("set_play")) {
    qint64 timeMs = 0;
    if (!parseInt64Field(obj, QStringLiteral("time"), &timeMs, &error)) {
      failProtocol(error);
      return;
    }
    emit playReceived(timeMs);
  } else if (action == QStringLiteral("set_pause")) {
    emit pauseReceived();
  } else if (action == QStringLiteral("set_stop")) {
    emit stopReceived();
  } else if (action == QStringLiteral("open_settings")) {
    emit openSettingsRequested();
  } else if (action == QStringLiteral("set_fullscreen")) {
    bool isFullscreen = false;
    if (!parseBoolField(obj, QStringLiteral("isFullscreen"), &isFullscreen, &error)) {
      failProtocol(error);
      return;
    }
    emit fullscreenReceived(isFullscreen);
  } else {
    failProtocol(QStringLiteral("unknown action '%1'").arg(action));
  }
}

void WsClient::dispatchIncomingBinary(const QByteArray& frame)
{
  // Spectrum frames are streaming snapshots: a bad one is dropped loudly,
  // but never tears down the connection (unlike malformed JSON, §7).
  if (frame.size() != kAnalyserFrameBytes) {
    qWarning() << "WsClient: dropping analyser frame of" << frame.size() << "bytes (expected"
               << kAnalyserFrameBytes << ")";
    return;
  }
  emit analyserDataReceived(frame);
}

void WsClient::failProtocol(const QString& reason)
{
  // Fail fast, fail loud (protocol §7): log and tear down the connection.
  // abort() drops the TCP link immediately — no half-open state while the
  // peer negotiates a close — so the app can never half-process this frame.
  qWarning() << "WsClient: protocol error, closing connection:" << reason;
  m_socket.abort();
}
