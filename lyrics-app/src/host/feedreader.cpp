/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "host/feedreader.h"

#include "host/tracklyrics.h"

#include <QDebug>
#include <QFileDevice>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>

namespace {

/// Protocol §2/§7: a line over 1 MiB is a protocol error.
constexpr qsizetype kMaxLineBytes = 1024 * 1024;
/// Protocol §5: the analyser frame is exactly 128 bytes.
constexpr int kAnalyserFrameBytes = 128;

/// qint64 bounds as doubles; both are exactly representable, so the comparison
/// below cannot round a legal value out of range (the smallest step between
/// doubles out here is 1024).
constexpr double kMinInt64 = -9223372036854775808.0; // -2^63
constexpr double kMaxInt64 = 9223372036854775808.0;  //  2^63, exclusive

/// A short, log-friendly rendering of whatever a malformed field carried, so a
/// rejected session names the value the host actually sent (e.g. `v="2"`).
QString describeJsonValue(const QJsonValue& value)
{
  switch (value.type()) {
  case QJsonValue::Null:
    return QStringLiteral("null");
  case QJsonValue::Bool:
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  case QJsonValue::Double:
    return QString::number(value.toDouble());
  case QJsonValue::String:
    return QStringLiteral("\"%1\"").arg(value.toString());
  case QJsonValue::Array:
    return QStringLiteral("an array");
  case QJsonValue::Object:
    return QStringLiteral("an object");
  case QJsonValue::Undefined:
    break;
  }
  return QStringLiteral("<missing>");
}

// Field parsing policy (protocol §6): a MISSING (or JSON null) field takes its
// empty/default value, so a host may omit what it does not know; a field that
// is PRESENT with the wrong type is a protocol violation. Typed parse, one
// place: everything downstream sees typed snapshots only.

bool parseStringField(const QJsonObject& obj, const QString& key, QString* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || value.isNull()) {
    out->clear();
    return true;
  }
  if (!value.isString()) {
    *error = QStringLiteral("'%1' is not a string").arg(key);
    return false;
  }
  *out = value.toString();
  return true;
}

bool parseBoolField(const QJsonObject& obj, const QString& key, bool* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || value.isNull()) {
    return true;
  }
  if (!value.isBool()) {
    *error = QStringLiteral("'%1' is not a boolean").arg(key);
    return false;
  }
  *out = value.toBool();
  return true;
}

bool parseInt64Field(const QJsonObject& obj, const QString& key, qint64* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || value.isNull()) {
    return true;
  }
  if (!value.isDouble()) {
    *error = QStringLiteral("'%1' is not a number").arg(key);
    return false;
  }
  // §6 types these fields (played_time, time, tempOffset) as integer
  // milliseconds and §7 makes a present-but-wrong field fatal — but
  // QJsonValue::toInteger() would silently answer 0 for 1.9 and for anything
  // past the qint64 range, reading a bad position as the start of the track.
  // Note Qt rejects `NaN`/`Infinity` literals and overflowing literals such as
  // 1e999 while parsing, so `number` is finite here; check anyway.
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::floor(number) != number || number < kMinInt64 ||
      number >= kMaxInt64) {
    *error = QStringLiteral("'%1' is not an integer: %2").arg(key).arg(number);
    return false;
  }
  *out = static_cast<qint64>(number);
  return true;
}

bool parseDoubleField(const QJsonObject& obj, const QString& key, double* out, QString* error)
{
  const QJsonValue value = obj.value(key);
  if (value.isUndefined() || value.isNull()) {
    return true;
  }
  if (!value.isDouble()) {
    *error = QStringLiteral("'%1' is not a number").arg(key);
    return false;
  }
  *out = value.toDouble();
  return true;
}

} // namespace

FeedReader::FeedReader(QIODevice* source, QIODevice* sink, QObject* parent)
  : QObject(parent)
  , m_source(source)
  , m_sink(sink)
{
  if (m_source == nullptr) {
    return; // Nothing to read: the session is inert (main() never does this).
  }

  connect(m_source, &QIODevice::readyRead, this, &FeedReader::readAvailable);
  connect(m_source, &QIODevice::readChannelFinished, this, [this] {
    readAvailable(); // whatever arrived together with the close
    if (m_exited || m_failed) {
      return;
    }
    m_exited = true;
    // §2: EOF on stdin means the player is gone — quit immediately, no
    // animation, no reconnect.
    qInfo() << "feed: host closed the pipe, exiting";
    emit exited();
  });
}

bool FeedReader::spectrumSupported() const
{
  return m_helloSeen && m_spectrum;
}

void FeedReader::requestAnalyserData()
{
  if (!spectrumSupported()) {
    return; // §5: only request frames when the host declared an analyser.
  }
  sendJson(QStringLiteral("get_analyser_data_array"), {});
}

void FeedReader::sendCloseRequested()
{
  // §4: the user closed the lyric window. The player ends its session and must
  // not treat the exit that follows as a crash.
  sendJson(QStringLiteral("close_requested"), {});
}

void FeedReader::readAvailable()
{
  if (m_failed || m_source == nullptr) {
    return;
  }

  m_buffer += m_source->readAll();

  forever
  {
    const qsizetype newline = m_buffer.indexOf('\n');
    if (newline < 0) {
      if (m_buffer.size() > kMaxLineBytes)
        failProtocol(
          QStringLiteral("host→app line exceeds %1 bytes").arg(static_cast<qint64>(kMaxLineBytes)));
      return;
    }
    if (newline > kMaxLineBytes) {
      failProtocol(
        QStringLiteral("host→app line exceeds %1 bytes").arg(static_cast<qint64>(kMaxLineBytes)));
      return;
    }

    QByteArray line = m_buffer.left(newline);
    m_buffer.remove(0, newline + 1);
    if (line.endsWith('\r'))
      line.chop(1);

    dispatchLine(line);
    if (m_failed)
      return; // Fail fast: never half-apply a broken session.
  }
}

void FeedReader::dispatchLine(const QByteArray& line)
{
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
  if (parseError.error != QJsonParseError::NoError) {
    failProtocol(QStringLiteral("malformed JSON: %1").arg(parseError.errorString()));
    return;
  }
  // Valid JSON of the wrong shape: a document that is not an object (a bare
  // array/scalar, or nothing at all) has no NoError to report above.
  if (!document.isObject()) {
    failProtocol(QStringLiteral("not a JSON object"));
    return;
  }

  const QJsonObject object = document.object();

  const QJsonValue version = object.value(QStringLiteral("v"));
  if (!version.isDouble() || version.toInt() != kProtocolVersion) {
    failProtocol(
      QStringLiteral("unsupported protocol version (v=%1)").arg(describeJsonValue(version)));
    return;
  }

  const QJsonValue actionValue = object.value(QStringLiteral("action"));
  if (!actionValue.isString() || actionValue.toString().isEmpty()) {
    failProtocol(QStringLiteral("missing action"));
    return;
  }

  const QString action = actionValue.toString();
  QString error;

  // §5 handshake: hello MUST be the first message; anything else coming first
  // means the host is not speaking v2 and no snapshot can be trusted.
  if (!m_helloSeen) {
    if (action != QStringLiteral("hello")) {
      failProtocol(QStringLiteral("first message is '%1', expected hello").arg(action));
      return;
    }
    QString host;
    bool spectrum = false;
    if (!parseStringField(object, QStringLiteral("host"), &host, &error) ||
        !parseBoolField(object, QStringLiteral("spectrum"), &spectrum, &error)) {
      failProtocol(QStringLiteral("invalid hello: %1").arg(error));
      return;
    }
    m_spectrum = spectrum;
    m_helloSeen = true;
    qInfo() << "feed: connected to" << (host.isEmpty() ? QStringLiteral("<unnamed host>") : host)
            << "spectrum:" << spectrum;
    return;
  }

  if (action == QStringLiteral("set_info")) {
    TrackSnapshot info;
    const bool ok =
      parseStringField(object, QStringLiteral("path"), &info.path, &error) &&
      parseStringField(object, QStringLiteral("singer"), &info.singer, &error) &&
      parseStringField(object, QStringLiteral("name"), &info.name, &error) &&
      parseStringField(object, QStringLiteral("album"), &info.album, &error) &&
      parseStringField(object, QStringLiteral("lrc"), &info.lrc, &error) &&
      parseStringField(object, QStringLiteral("tlrc"), &info.tlrc, &error) &&
      parseStringField(object, QStringLiteral("rlrc"), &info.rlrc, &error) &&
      parseStringField(object, QStringLiteral("lxlrc"), &info.lxlrc, &error) &&
      parseBoolField(object, QStringLiteral("isPlay"), &info.isPlay, &error) &&
      parseInt64Field(object, QStringLiteral("played_time"), &info.playedTimeMs, &error);
    if (!ok) {
      failProtocol(QStringLiteral("invalid set_info: %1").arg(error));
      return;
    }

    // §5 lyric precedence: a non-empty host `lrc` wins; otherwise the app owns
    // acquisition and reads the file at `path` (sidecar first, then embedded
    // tags) — the same contract the player-side adapters implement.
    if (info.lrc.isEmpty())
      info.lrc = lyricsForTrackFile(info.path);

    qInfo() << "feed: track" << (info.name.isEmpty() ? QStringLiteral("<unknown>") : info.name)
            << (info.singer.isEmpty() ? QString() : QStringLiteral("- %1").arg(info.singer))
            << (info.path.isEmpty() ? QString() : QStringLiteral("[%1]").arg(info.path))
            << "playing:" << info.isPlay << "lyric chars:" << info.lrc.size();
    emit infoReceived(info);
    return;
  }

  if (action == QStringLiteral("set_lyric")) {
    LyricSnapshot lyric;
    const bool ok = parseStringField(object, QStringLiteral("lrc"), &lyric.lrc, &error) &&
                    parseStringField(object, QStringLiteral("tlrc"), &lyric.tlrc, &error) &&
                    parseStringField(object, QStringLiteral("rlrc"), &lyric.rlrc, &error) &&
                    parseStringField(object, QStringLiteral("lxlrc"), &lyric.lxlrc, &error);
    if (!ok) {
      failProtocol(QStringLiteral("invalid set_lyric: %1").arg(error));
      return;
    }
    emit lyricReceived(lyric);
    return;
  }

  if (action == QStringLiteral("set_status")) {
    PlaybackSnapshot status;
    const bool ok =
      parseBoolField(object, QStringLiteral("isPlay"), &status.isPlay, &error) &&
      parseInt64Field(object, QStringLiteral("played_time"), &status.playedTimeMs, &error);
    if (!ok) {
      failProtocol(QStringLiteral("invalid set_status: %1").arg(error));
      return;
    }
    emit statusReceived(status);
    return;
  }

  if (action == QStringLiteral("set_offset")) {
    qint64 tempOffset = 0;
    if (!parseInt64Field(object, QStringLiteral("tempOffset"), &tempOffset, &error)) {
      failProtocol(QStringLiteral("invalid set_offset: %1").arg(error));
      return;
    }
    emit offsetReceived(tempOffset);
    return;
  }

  if (action == QStringLiteral("set_playbackRate")) {
    double rate = 1.0;
    if (!parseDoubleField(object, QStringLiteral("rate"), &rate, &error)) {
      failProtocol(QStringLiteral("invalid set_playbackRate: %1").arg(error));
      return;
    }
    // The 0.25–4.0 rule lives in the lyric pipeline, not here.
    emit playbackRateReceived(rate);
    return;
  }

  if (action == QStringLiteral("set_play")) {
    qint64 timeMs = 0;
    if (!parseInt64Field(object, QStringLiteral("time"), &timeMs, &error)) {
      failProtocol(QStringLiteral("invalid set_play: %1").arg(error));
      return;
    }
    emit playReceived(timeMs);
    return;
  }

  if (action == QStringLiteral("set_pause")) {
    emit pauseReceived();
    return;
  }

  if (action == QStringLiteral("set_stop")) {
    emit stopReceived();
    return;
  }

  if (action == QStringLiteral("open_settings")) {
    emit openSettingsRequested();
    return;
  }

  if (action == QStringLiteral("set_fullscreen")) {
    bool isFullscreen = false;
    if (!parseBoolField(object, QStringLiteral("isFullscreen"), &isFullscreen, &error)) {
      failProtocol(QStringLiteral("invalid set_fullscreen: %1").arg(error));
      return;
    }
    emit fullscreenReceived(isFullscreen);
    return;
  }

  if (action == QStringLiteral("spectrum")) {
    QString encoded;
    if (!parseStringField(object, QStringLiteral("data"), &encoded, &error)) {
      failProtocol(QStringLiteral("invalid spectrum: %1").arg(error));
      return;
    }
    // Spectrum frames are streaming snapshots (§5): a wrong-sized one is
    // dropped loudly, but never tears the session down.
    const QByteArray frame = QByteArray::fromBase64(encoded.toLatin1());
    if (frame.size() != kAnalyserFrameBytes) {
      qWarning() << QStringLiteral("feed: dropping analyser frame of %1 bytes (expected %2)")
                      .arg(frame.size())
                      .arg(kAnalyserFrameBytes);
      return;
    }
    emit analyserDataReceived(frame);
    return;
  }

  failProtocol(QStringLiteral("unknown action '%1'").arg(action));
}

void FeedReader::failProtocol(const QString& reason)
{
  if (m_failed) {
    return;
  }
  m_failed = true;
  qWarning() << "feed: protocol error:" << reason;
  emit protocolError(reason);
}

void FeedReader::sendJson(const QString& action, const QJsonObject& fields)
{
  // Once the host closed its end (§2 EOF) the app is on its way out and there
  // is nobody left to read app→host lines. This is what keeps the window's
  // shutdown close event from being reported as §4 close_requested — a close
  // the PLAYER initiated must never look like the user closing the window.
  if (m_sink == nullptr || m_failed || m_exited) {
    return;
  }

  // The sink already refused a line (the host stopped draining its stdout, or
  // its reader is gone): the device is latched broken, so warn once — here —
  // and then stay silent instead of repeating it per spectrum frame.
  if (m_sinkBroken) {
    return;
  }

  QJsonObject message{{QStringLiteral("v"), kProtocolVersion}, {QStringLiteral("action"), action}};
  for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
    message.insert(it.key(), it.value());
  }

  const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  if (m_sink->write(line) != line.size()) {
    m_sinkBroken = true;
    qWarning() << "feed: cannot deliver to the host (app→host lines dropped from here on):"
               << action;
    return;
  }

  // A QFileDevice-backed sink buffers; flush so the host sees the line now.
  if (auto* file = qobject_cast<QFileDevice*>(m_sink); file != nullptr) {
    file->flush();
  }
}
