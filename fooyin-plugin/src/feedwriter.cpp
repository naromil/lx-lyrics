/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */
#include "feedwriter.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStandardPaths>

namespace {

/// Protocol §2: a line over 1 MiB is a protocol error.
constexpr qsizetype kMaxLineBytes = 1024 * 1024;
/// The app is a Qt process that execs immediately; a launch that has not
/// started by now failed. Kept short because spawn() runs on the GUI thread.
constexpr int kStartTimeoutMs = 1500;
/// Graceful teardown budget: the app quits on the stdin EOF, so this covers
/// its shutdown path (config flush, window teardown).
constexpr int kStopTimeoutMs = 1000;
/// Escalation budget after terminate() — and after kill() in failProtocol().
constexpr int kTerminateTimeoutMs = 250;

} // namespace

FeedWriter::FeedWriter(QObject* parent)
  : QObject(parent)
  , m_process(new QProcess(this))
{
  // Three pipes, child never detached (§2): the lyric window lives and dies
  // with the player process. SeparateChannels keeps the app's free-form logs
  // (stderr) out of the protocol stream (stdout).
  m_process->setProcessChannelMode(QProcess::SeparateChannels);

  connect(m_process, &QProcess::readyReadStandardOutput, this,
          &FeedWriter::onReadyReadStandardOutput);
  connect(m_process, &QProcess::readyReadStandardError, this,
          &FeedWriter::onReadyReadStandardError);
  connect(m_process, &QProcess::finished, this, &FeedWriter::onFinished);
}

FeedWriter::~FeedWriter()
{
  stop();
}

void FeedWriter::setAppPath(const QString& path)
{
  m_appPath = path;
}

void FeedWriter::setSpectrumAvailable(bool available)
{
  m_spectrumAvailable = available;
}

bool FeedWriter::spawn()
{
  if (isRunning()) {
    qWarning() << "[LX Lyrics] lyrics-app already running; skipping duplicate launch";
    return false;
  }

  const QString program = resolveAppPath();
  if (program.isEmpty()) {
    qWarning() << "[LX Lyrics] lx-lyrics-app binary not found (PATH / fooyin bin dir)";
    return false;
  }

  m_stdoutBuffer.clear();
  m_closeRequested = false;
  m_protocolFailed = false;
  m_stopping = false;

  // Protocol §8: argv array, no shell interpolation.
  m_process->start(program, {QStringLiteral("--player-feed")});
  if (!m_process->waitForStarted(kStartTimeoutMs)) {
    qWarning() << "[LX Lyrics] failed to start lyrics-app:" << program << m_process->errorString();
    m_process->kill();
    m_process->waitForFinished(kTerminateTimeoutMs);
    return false;
  }

  // §2 handshake: hello MUST be the first line the app reads. It is written
  // before spawn() returns, so the snapshot the plugin pushes right after
  // spawn() queues behind it in the pipe — the spawn race does not exist.
  sendJson(QStringLiteral("hello"), {{QStringLiteral("host"), QStringLiteral("fooyin")},
                                     {QStringLiteral("spectrum"), m_spectrumAvailable}});

  qInfo() << "[LX Lyrics] spawned lyrics-app:" << program << QStringLiteral("--player-feed");
  emit appStarted();
  return true;
}

void FeedWriter::stop()
{
  if (m_process->state() == QProcess::NotRunning) {
    return;
  }

  m_stopping = true;

  // §2: EOF on the app's stdin is the quit request (no animation), so close
  // the write channel first and only escalate if the app does not comply.
  m_process->closeWriteChannel();
  if (!m_process->waitForFinished(kStopTimeoutMs)) {
    m_process->terminate();
    if (!m_process->waitForFinished(kTerminateTimeoutMs)) {
      m_process->kill();
      m_process->waitForFinished(kTerminateTimeoutMs);
    }
  }

  // onFinished() has already cleared these when the child really finished;
  // clearing again keeps the state honest when it did not.
  m_stdoutBuffer.clear();
  m_closeRequested = false;
  m_protocolFailed = false;
  m_stopping = false;
}

bool FeedWriter::isRunning() const
{
  return m_process->state() != QProcess::NotRunning;
}

int FeedWriter::exitCode() const
{
  return m_process->exitCode();
}

void FeedWriter::sendSetInfo(const QVariantMap& fields)
{
  sendJson(QStringLiteral("set_info"), QJsonObject::fromVariantMap(fields));
}

void FeedWriter::sendSetLyric(const QString& lrc, const QString& tlrc, const QString& rlrc,
                              const QString& lxlrc)
{
  sendJson(QStringLiteral("set_lyric"), {{QStringLiteral("lrc"), lrc},
                                         {QStringLiteral("tlrc"), tlrc},
                                         {QStringLiteral("rlrc"), rlrc},
                                         {QStringLiteral("lxlrc"), lxlrc}});
}

void FeedWriter::sendSetStatus(bool isPlay, qint64 playedTime)
{
  sendJson(QStringLiteral("set_status"),
           {{QStringLiteral("isPlay"), isPlay}, {QStringLiteral("played_time"), playedTime}});
}

void FeedWriter::sendSetOffset(qint64 tempOffset)
{
  sendJson(QStringLiteral("set_offset"), {{QStringLiteral("tempOffset"), tempOffset}});
}

void FeedWriter::sendSetPlaybackRate(double rate)
{
  sendJson(QStringLiteral("set_playbackRate"), {{QStringLiteral("rate"), rate}});
}

void FeedWriter::sendSetPlay(qint64 time)
{
  sendJson(QStringLiteral("set_play"), {{QStringLiteral("time"), time}});
}

void FeedWriter::sendSetPause()
{
  sendJson(QStringLiteral("set_pause"), {});
}

void FeedWriter::sendSetStop()
{
  sendJson(QStringLiteral("set_stop"), {});
}

void FeedWriter::sendSetFullscreen(bool isFullscreen)
{
  sendJson(QStringLiteral("set_fullscreen"), {{QStringLiteral("isFullscreen"), isFullscreen}});
}

void FeedWriter::sendOpenSettings()
{
  sendJson(QStringLiteral("open_settings"), {});
}

void FeedWriter::sendAnalyserData(const QByteArray& data)
{
  // §5: the frame is exactly 128 bytes; anything else would be dropped by the
  // app, so refuse it here where the bug actually is.
  if (data.size() != 128) {
    qWarning() << "[LX Lyrics] refusing to send analyser frame:" << data.size()
               << "bytes (protocol requires exactly 128)";
    return;
  }
  sendJson(QStringLiteral("spectrum"),
           {{QStringLiteral("data"), QString::fromLatin1(data.toBase64())}});
}

void FeedWriter::onReadyReadStandardOutput()
{
  m_stdoutBuffer += m_process->readAllStandardOutput();

  forever
  {
    const qsizetype newline = m_stdoutBuffer.indexOf('\n');
    if (newline < 0) {
      if (m_stdoutBuffer.size() > kMaxLineBytes)
        failProtocol(
          QStringLiteral("app→host line exceeds %1 bytes").arg(static_cast<qint64>(kMaxLineBytes)));
      return;
    }
    if (newline > kMaxLineBytes) {
      failProtocol(
        QStringLiteral("app→host line exceeds %1 bytes").arg(static_cast<qint64>(kMaxLineBytes)));
      return;
    }

    QByteArray line = m_stdoutBuffer.left(newline);
    m_stdoutBuffer.remove(0, newline + 1);
    if (line.endsWith('\r'))
      line.chop(1);

    dispatchLine(line);
    if (m_protocolFailed)
      return; // the session already ended; no cascade of errors.
  }
}

void FeedWriter::onReadyReadStandardError()
{
  const QByteArray chunk = m_process->readAllStandardError();
  if (chunk.isEmpty())
    return;

  // The app's logs are free-form (§2), and draining them is also what keeps
  // the child from blocking once the stderr pipe buffer fills.
  const QList<QByteArray> lines = chunk.split('\n');
  for (const QByteArray& line : lines) {
    if (!line.isEmpty())
      qInfo().noquote() << QStringLiteral("[LX Lyrics app]") << QString::fromUtf8(line);
  }
}

void FeedWriter::onFinished(int code, QProcess::ExitStatus status)
{
  onReadyReadStandardOutput(); // whatever the child wrote just before exiting
  onReadyReadStandardError();

  const bool closeRequested = m_closeRequested;
  const bool wasStopping = m_stopping;

  // A crash, or a clean exit from a session that violated the protocol, is not
  // a successful run: report non-zero so the plugin unchecks the toggle
  // instead of respawning a broken app.
  const int reported = (status == QProcess::NormalExit && !m_protocolFailed) ? code : -1;

  m_stdoutBuffer.clear();
  m_closeRequested = false;
  m_protocolFailed = false;
  m_stopping = false;

  if (wasStopping)
    return; // plugin-initiated teardown: not a lost session.

  emit appExited(reported, closeRequested);
}

void FeedWriter::dispatchLine(const QByteArray& line)
{
  // Strict, line-wise (§7): the app's stdout carries protocol lines only, so
  // noise is a protocol error, not something to skip past.
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    failProtocol(QStringLiteral("malformed JSON: %1").arg(parseError.errorString()));
    return;
  }

  const QJsonObject object = document.object();

  const QJsonValue version = object.value(QStringLiteral("v"));
  if (!version.isDouble() || version.toInt() != kProtocolVersion) {
    failProtocol(QStringLiteral("unsupported protocol version (v=%1)")
                   .arg(version.isDouble() ? QString::number(version.toDouble())
                                           : QStringLiteral("<missing>")));
    return;
  }

  const QJsonValue actionValue = object.value(QStringLiteral("action"));
  if (!actionValue.isString() || actionValue.toString().isEmpty()) {
    failProtocol(QStringLiteral("missing action"));
    return;
  }

  const QString action = actionValue.toString();
  if (action == QStringLiteral("get_analyser_data_array")) {
    emit analyserDataRequested();
  } else if (action == QStringLiteral("close_requested")) {
    // §4: the user closed the lyric window. The plugin ends its session
    // without respawning; the exit that follows is reported with
    // closeRequested=true.
    m_closeRequested = true;
    emit closeRequested();
  } else {
    failProtocol(QStringLiteral("unknown action '%1'").arg(action));
  }
}

void FeedWriter::failProtocol(const QString& reason)
{
  if (m_protocolFailed) {
    return;
  }
  m_protocolFailed = true;

  qWarning() << "[LX Lyrics] protocol error from lyrics-app:" << reason;

  if (m_process->state() == QProcess::NotRunning) {
    return; // Already gone: onFinished() is what reports the failed session.
  }

  // The session is unusable: end it (a child left writing into a pipe nobody
  // parses would just keep producing garbage). onFinished() then reports the
  // non-zero status that makes the plugin uncheck its toggle.
  m_process->terminate();
  if (!m_process->waitForFinished(kTerminateTimeoutMs))
    m_process->kill();
}

QString FeedWriter::resolveAppPath() const
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

void FeedWriter::sendJson(const QString& action, const QJsonObject& fields)
{
  if (!isRunning()) {
    return; // No child; drop like the plugin's other state pushes.
  }

  QJsonObject message{{QStringLiteral("v"), kProtocolVersion}, {QStringLiteral("action"), action}};
  for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
    message.insert(it.key(), it.value());
  }

  const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  if (m_process->write(line) != line.size()) {
    qWarning() << "[LX Lyrics] short write to lyrics-app stdin:" << action;
  }

  // One line per write, flushed: waitForBytesWritten(0) makes one immediate
  // write attempt, so a line cannot sit in QProcess' buffer waiting for a
  // later event-loop turn (the plugin's main thread may be busy).
  m_process->waitForBytesWritten(0);
}
