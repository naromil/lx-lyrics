/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>

#include "bridge/wsclient.h"
#include "hostserver.h"

// Round-trip tests for the WebSocket bridge (docs/protocol.md).
//
// A real QWebSocketServer stub host runs on an ephemeral loopback port so the
// wire messages are observed and driven exactly as the Fooyin host would:
// text frames are JSON (§5), the only binary frame is the 128-byte analyser
// snapshot, and a malformed/unknown/bad-version frame must fail fast and tear
// the connection down (§7).
//
// The client is built with a deliberately long reconnect interval: the
// reconnect timer (2000ms by default) is not under test, and a long interval
// removes any chance of it firing mid-assertion on a slow CI machine.

namespace {

constexpr int kLongReconnectIntervalMs = 60000;
constexpr int kWaitMs = 3000;

// Compact JSON text frame for the host→app direction (§5).
QString textFrame(const QJsonObject& obj)
{
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

} // namespace

// A WebSocket host stub: accepts exactly one client (protocol §8) and can push
// text/binary frames at it. Frames the app sends are re-emitted as signals so a
// test can assert on the wire format.
class StubHost : public QObject {
  Q_OBJECT

public:
  explicit StubHost(QObject* parent = nullptr)
    : QObject(parent)
    , m_server(QStringLiteral("lyrics-app-test-host"), QWebSocketServer::NonSecureMode)
  {
    if (!m_server.listen(QHostAddress::LocalHost, 0))
      qFatal("StubHost: cannot listen on an ephemeral loopback port");
    connect(&m_server, &QWebSocketServer::newConnection, this, &StubHost::onNewConnection);
  }

  ~StubHost() override
  {
    m_server.close();
    if (m_client) {
      QWebSocket* socket = m_client;
      m_client = nullptr; // the disconnect handler must not re-touch it
      socket->abort();
      delete socket;
    }
  }

  QUrl url() const { return m_server.serverUrl(); }
  bool hasClient() const { return m_client != nullptr; }

  void sendText(const QString& frame)
  {
    if (m_client && m_client->isValid())
      m_client->sendTextMessage(frame);
  }

  void sendBinary(const QByteArray& frame)
  {
    if (m_client && m_client->isValid())
      m_client->sendBinaryMessage(frame);
  }

  // Graceful close of the app-facing socket: what a host shutdown looks like.
  void closeClient()
  {
    if (m_client)
      m_client->close();
  }

signals:
  // moc generates the signal definition with its own parameter names (_t1);
  // the names here are intentional.
  // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
  void textFrameReceived(const QString& frame);
  // NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name) -- see above.
  void binaryFrameReceived(const QByteArray& frame);
  void clientConnected();
  void clientDisconnected();

private:
  void onNewConnection()
  {
    while (m_server.hasPendingConnections()) {
      QWebSocket* socket = m_server.nextPendingConnection();
      if (m_client) {
        socket->abort(); // §8: exactly one client; reject extras.
        socket->deleteLater();
        continue;
      }
      m_client = socket;
      connect(socket, &QWebSocket::textMessageReceived, this, &StubHost::textFrameReceived);
      connect(socket, &QWebSocket::binaryMessageReceived, this, &StubHost::binaryFrameReceived);
      connect(socket, &QWebSocket::disconnected, this, [this, socket] {
        if (m_client == socket) {
          m_client = nullptr;
          socket->deleteLater();
          clientDisconnected();
        }
      });
      clientConnected();
    }
  }

  QWebSocketServer m_server;
  QWebSocket* m_client = nullptr;
};

class TestProtocol : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();

  void clientConnectsAndSendsGetInfo();
  void hostSetInfoFullPayload();
  void hostSetLyricAndStatus();
  void hostPlayPauseStopOffsetRate();
  void hostSetFullscreen();
  void hostServerSendsSetFullscreen();
  void binaryAnalyserFrame();
  void clientSendsCloseRequested();
  void hostServerRecognizesCloseRequested();
  void hostServerProtocolErrorEmitsDedicatedSignal();
  void malformedJsonFailsFast();
  void unknownActionFailsFast();
  void badVersionFailsFast();
  void exitOnDisconnectSemantics();

  // moc requires an explicit access specifier to terminate the private slots region;
  // without it the members below are parsed as slot declarations and moc fails.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
private:
  StubHost* m_host = nullptr;
  WsClient* m_client = nullptr;
};

void TestProtocol::init()
{
  m_host = new StubHost;
  m_client = new WsClient(kLongReconnectIntervalMs);
}

void TestProtocol::cleanup()
{
  if (m_client) {
    m_client->disconnectFromHost(); // stops reconnecting, closes the socket
    delete m_client;
    m_client = nullptr;
  }
  delete m_host;
  m_host = nullptr;
}

void TestProtocol::clientConnectsAndSendsGetInfo()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy hostClientSpy(m_host, &StubHost::clientConnected);
  QSignalSpy frameSpy(m_host, &StubHost::textFrameReceived);

  m_client->connectToHost(m_host->url());

  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);
  QTRY_COMPARE_WITH_TIMEOUT(hostClientSpy.count(), 1, kWaitMs);

  m_client->sendGetInfo();

  QTRY_COMPARE_WITH_TIMEOUT(frameSpy.count(), 1, kWaitMs);
  const QJsonDocument doc = QJsonDocument::fromJson(frameSpy.at(0).at(0).toString().toUtf8());
  QVERIFY(doc.isObject());
  const QJsonObject obj = doc.object();
  QCOMPARE(obj.value(QStringLiteral("v")).toInt(), 1);
  QCOMPARE(obj.value(QStringLiteral("action")).toString(), QStringLiteral("get_info"));
}

void TestProtocol::hostSetInfoFullPayload()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy infoSpy(m_client, &WsClient::infoReceived);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_info")},
    {QStringLiteral("id"), QStringLiteral("t1")},
    {QStringLiteral("singer"), QStringLiteral("Singer")},
    {QStringLiteral("name"), QStringLiteral("Name")},
    {QStringLiteral("album"), QStringLiteral("Album")},
    {QStringLiteral("lrc"), QStringLiteral("[00:01.00]line one\n[00:02.00]line two")},
    {QStringLiteral("tlrc"), QStringLiteral("第一行")},
    {QStringLiteral("rlrc"), QStringLiteral("Romaji")},
    {QStringLiteral("lxlrc"), QStringLiteral("<0,9999>Hello")},
    {QStringLiteral("isPlay"), true},
    {QStringLiteral("line"), -1},
    {QStringLiteral("played_time"), 12340},
  }));

  QTRY_COMPARE_WITH_TIMEOUT(infoSpy.count(), 1, kWaitMs);
  const TrackSnapshot snapshot = infoSpy.at(0).at(0).value<TrackSnapshot>();
  QCOMPARE(snapshot.id, QStringLiteral("t1"));
  QCOMPARE(snapshot.singer, QStringLiteral("Singer"));
  QCOMPARE(snapshot.name, QStringLiteral("Name"));
  QCOMPARE(snapshot.album, QStringLiteral("Album"));
  QCOMPARE(snapshot.lrc, QStringLiteral("[00:01.00]line one\n[00:02.00]line two"));
  QCOMPARE(snapshot.tlrc, QStringLiteral("第一行"));
  QCOMPARE(snapshot.rlrc, QStringLiteral("Romaji"));
  QCOMPARE(snapshot.lxlrc, QStringLiteral("<0,9999>Hello"));
  QCOMPARE(snapshot.isPlay, true);
  QCOMPARE(snapshot.line, qint64(-1));
  QCOMPARE(snapshot.playedTimeMs, qint64(12340));
}

void TestProtocol::hostSetLyricAndStatus()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy lyricSpy(m_client, &WsClient::lyricReceived);
  QSignalSpy statusSpy(m_client, &WsClient::statusReceived);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_lyric")},
    {QStringLiteral("lrc"), QStringLiteral("[00:01.00]A")},
    {QStringLiteral("tlrc"), QStringLiteral("译")},
    {QStringLiteral("rlrc"), QStringLiteral("R")},
    {QStringLiteral("lxlrc"), QStringLiteral("<0,9999>Hi")},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(lyricSpy.count(), 1, kWaitMs);
  const LyricSnapshot lyric = lyricSpy.at(0).at(0).value<LyricSnapshot>();
  QCOMPARE(lyric.lrc, QStringLiteral("[00:01.00]A"));
  QCOMPARE(lyric.tlrc, QStringLiteral("译"));
  QCOMPARE(lyric.rlrc, QStringLiteral("R"));
  QCOMPARE(lyric.lxlrc, QStringLiteral("<0,9999>Hi"));

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_status")},
    {QStringLiteral("isPlay"), false},
    {QStringLiteral("line"), 3},
    {QStringLiteral("played_time"), 5000},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(statusSpy.count(), 1, kWaitMs);
  const PlaybackSnapshot status = statusSpy.at(0).at(0).value<PlaybackSnapshot>();
  QCOMPARE(status.isPlay, false);
  QCOMPARE(status.line, qint64(3));
  QCOMPARE(status.playedTimeMs, qint64(5000));
}

void TestProtocol::hostPlayPauseStopOffsetRate()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy playSpy(m_client, &WsClient::playReceived);
  QSignalSpy pauseSpy(m_client, &WsClient::pauseReceived);
  QSignalSpy stopSpy(m_client, &WsClient::stopReceived);
  QSignalSpy offsetSpy(m_client, &WsClient::offsetReceived);
  QSignalSpy rateSpy(m_client, &WsClient::playbackRateReceived);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_play")},
    {QStringLiteral("time"), 1234},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(playSpy.count(), 1, kWaitMs);
  QCOMPARE(playSpy.at(0).at(0).toLongLong(), qint64(1234));

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_pause")},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(pauseSpy.count(), 1, kWaitMs);

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_stop")},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(stopSpy.count(), 1, kWaitMs);

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_offset")},
    {QStringLiteral("tempOffset"), -200},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(offsetSpy.count(), 1, kWaitMs);
  QCOMPARE(offsetSpy.at(0).at(0).toLongLong(), qint64(-200));

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_playbackRate")},
    {QStringLiteral("rate"), 2.0},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(rateSpy.count(), 1, kWaitMs);
  QCOMPARE(rateSpy.at(0).at(0).toDouble(), 2.0);
}

void TestProtocol::hostSetFullscreen()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy fullscreenSpy(m_client, &WsClient::fullscreenReceived);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_fullscreen")},
    {QStringLiteral("isFullscreen"), true},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(fullscreenSpy.count(), 1, kWaitMs);
  QCOMPARE(fullscreenSpy.at(0).at(0).toBool(), true);

  m_host->sendText(textFrame({
    {QStringLiteral("v"), 1},
    {QStringLiteral("action"), QStringLiteral("set_fullscreen")},
    {QStringLiteral("isFullscreen"), false},
  }));
  QTRY_COMPARE_WITH_TIMEOUT(fullscreenSpy.count(), 2, kWaitMs);
  QCOMPARE(fullscreenSpy.at(1).at(0).toBool(), false);
}

void TestProtocol::hostServerSendsSetFullscreen()
{
  // The host side of the same contract: HostServer must emit exactly
  // {"v":1,"action":"set_fullscreen","isFullscreen":bool}. HostServer has no
  // Fooyin dependencies, so it is compiled into this test target and driven
  // through a real QWebSocket client (round-trip on the wire).
  HostServer host;
  QVERIFY(host.isListening());
  QSignalSpy clientConnectedSpy(&host, &HostServer::clientConnected);

  QWebSocket client;
  QSignalSpy clientSocketSpy(&client, &QWebSocket::connected);
  QSignalSpy frameSpy(&client, &QWebSocket::textMessageReceived);

  client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(host.serverPort())));
  QTRY_COMPARE_WITH_TIMEOUT(clientConnectedSpy.count(), 1, kWaitMs);
  QTRY_COMPARE_WITH_TIMEOUT(clientSocketSpy.count(), 1, kWaitMs);

  host.sendSetFullscreen(true);
  QTRY_COMPARE_WITH_TIMEOUT(frameSpy.count(), 1, kWaitMs);
  const QJsonObject first =
    QJsonDocument::fromJson(frameSpy.at(0).at(0).toString().toUtf8()).object();
  QCOMPARE(first.value(QStringLiteral("v")).toInt(), 1);
  QCOMPARE(first.value(QStringLiteral("action")).toString(), QStringLiteral("set_fullscreen"));
  QCOMPARE(first.value(QStringLiteral("isFullscreen")).toBool(), true);

  host.sendSetFullscreen(false);
  QTRY_COMPARE_WITH_TIMEOUT(frameSpy.count(), 2, kWaitMs);
  const QJsonObject second =
    QJsonDocument::fromJson(frameSpy.at(1).at(0).toString().toUtf8()).object();
  QCOMPARE(second.value(QStringLiteral("isFullscreen")).toBool(), false);
}

void TestProtocol::binaryAnalyserFrame()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy analyserSpy(m_client, &WsClient::analyserDataReceived);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  QByteArray frame(128, 0);
  for (int i = 0; i < frame.size(); ++i)
    frame[i] = char(i & 0xff);
  m_host->sendBinary(frame);

  QTRY_COMPARE_WITH_TIMEOUT(analyserSpy.count(), 1, kWaitMs);
  QCOMPARE(analyserSpy.at(0).at(0).toByteArray(), frame);

  // A 64-byte frame must be dropped (§7): no signal may fire. Sending a valid
  // frame right after proves the bad one was processed and skipped — the valid
  // frame lands at index 1, so index 0 is still the first, 128-byte frame.
  m_host->sendBinary(QByteArray(64, '\xAB'));
  m_host->sendBinary(frame);

  QTRY_COMPARE_WITH_TIMEOUT(analyserSpy.count(), 2, kWaitMs);
  QCOMPARE(analyserSpy.at(0).at(0).toByteArray(), frame);
  QCOMPARE(analyserSpy.at(1).at(0).toByteArray(), frame);
}

void TestProtocol::clientSendsCloseRequested()
{
  // App side of §4 close_requested: exactly {"v":1,"action":"close_requested"}
  // with no payload, sent when the user closes the lyric window.
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy frameSpy(m_host, &StubHost::textFrameReceived);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_client->sendCloseRequested();

  QTRY_COMPARE_WITH_TIMEOUT(frameSpy.count(), 1, kWaitMs);
  const QJsonDocument doc = QJsonDocument::fromJson(frameSpy.at(0).at(0).toString().toUtf8());
  QVERIFY(doc.isObject());
  const QJsonObject obj = doc.object();
  QCOMPARE(obj.value(QStringLiteral("v")).toInt(), 1);
  QCOMPARE(obj.value(QStringLiteral("action")).toString(), QStringLiteral("close_requested"));
  QCOMPARE(obj.size(), 2); // v + action only: no payload.

  // Harmless when disconnected (the host may already be gone): dropped, no
  // frame, no crash, and the reconnect path is untouched.
  m_host->closeClient();
  QTRY_VERIFY_WITH_TIMEOUT(!m_client->isConnected(), kWaitMs);
  const int framesBefore = frameSpy.count();
  m_client->sendCloseRequested();
  QTest::qWait(100);
  QCOMPARE(frameSpy.count(), framesBefore); // Dropped, not sent.
}

void TestProtocol::hostServerRecognizesCloseRequested()
{
  // Host side of §4 close_requested: HostServer must emit closeRequested —
  // a KNOWN action, not a protocol error (the connection must survive).
  // The plugin DEFERS its teardown to a queued callback because this signal
  // fires while HostServer is still inside its message handler: the emitting
  // server must survive its own closeRequested emission — still listening and
  // still serving the connected client — until the plugin's deferred teardown
  // runs (which is not under test here, only the contract it relies on).
  HostServer host;
  QVERIFY(host.isListening());
  QSignalSpy clientConnectedSpy(&host, &HostServer::clientConnected);
  QSignalSpy closeSpy(&host, &HostServer::closeRequested);
  QSignalSpy protocolErrorSpy(&host, &HostServer::protocolErrorClosed);
  QSignalSpy clientDisconnectedSpy(&host, &HostServer::clientDisconnected);

  QWebSocket client;
  QSignalSpy clientSocketSpy(&client, &QWebSocket::connected);
  QSignalSpy frameSpy(&client, &QWebSocket::textMessageReceived);
  client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(host.serverPort())));
  QTRY_COMPARE_WITH_TIMEOUT(clientConnectedSpy.count(), 1, kWaitMs);
  QTRY_COMPARE_WITH_TIMEOUT(clientSocketSpy.count(), 1, kWaitMs);

  client.sendTextMessage(QStringLiteral("{\"v\":1,\"action\":\"close_requested\"}"));
  QTRY_COMPARE_WITH_TIMEOUT(closeSpy.count(), 1, kWaitMs);

  // Not a protocol error: no dedicated error signal, no disconnect, and the
  // socket stays up (the plugin decides what to do with the signal).
  QTest::qWait(100);
  QCOMPARE(protocolErrorSpy.count(), 0);
  QCOMPARE(clientDisconnectedSpy.count(), 0);
  QCOMPARE(client.state(), QAbstractSocket::ConnectedState);

  // The sender survives its own emission: still listening, and a host->app
  // frame still reaches the connected client after the signal returned.
  QVERIFY(host.isListening());
  host.sendSetFullscreen(true);
  QTRY_COMPARE_WITH_TIMEOUT(frameSpy.count(), 1, kWaitMs);
  const QJsonObject frame =
    QJsonDocument::fromJson(frameSpy.at(0).at(0).toString().toUtf8()).object();
  QCOMPARE(frame.value(QStringLiteral("action")).toString(), QStringLiteral("set_fullscreen"));
  QCOMPARE(client.state(), QAbstractSocket::ConnectedState);
}

void TestProtocol::hostServerProtocolErrorEmitsDedicatedSignal()
{
  // §7 protocol-error close (task C): HostServer emits the DEDICATED
  // protocolErrorClosed signal — never clientDisconnected — and keeps
  // listening so a corrected client may reconnect.
  HostServer host;
  QVERIFY(host.isListening());
  QSignalSpy clientConnectedSpy(&host, &HostServer::clientConnected);
  QSignalSpy protocolErrorSpy(&host, &HostServer::protocolErrorClosed);
  QSignalSpy clientDisconnectedSpy(&host, &HostServer::clientDisconnected);

  QWebSocket client;
  QSignalSpy clientSocketSpy(&client, &QWebSocket::connected);
  client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(host.serverPort())));
  QTRY_COMPARE_WITH_TIMEOUT(clientConnectedSpy.count(), 1, kWaitMs);
  QTRY_COMPARE_WITH_TIMEOUT(clientSocketSpy.count(), 1, kWaitMs);

  client.sendTextMessage(QStringLiteral("{not json"));
  QTRY_COMPARE_WITH_TIMEOUT(protocolErrorSpy.count(), 1, kWaitMs);

  // The close is a malformed-client event: the normal clientDisconnected
  // signal must NOT fire (the plugin's crash-respawn must not trigger), and
  // the server keeps listening for a reconnect.
  QTest::qWait(100);
  QCOMPARE(clientDisconnectedSpy.count(), 0);

  // A replacement client can reconnect and is accepted (§7 recovery).
  QWebSocket replacement;
  QSignalSpy replacementSocketSpy(&replacement, &QWebSocket::connected);
  replacement.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(host.serverPort())));
  QTRY_COMPARE_WITH_TIMEOUT(clientConnectedSpy.count(), 2, kWaitMs);
  QTRY_COMPARE_WITH_TIMEOUT(replacementSocketSpy.count(), 1, kWaitMs);
}

void TestProtocol::malformedJsonFailsFast()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy disconnectedSpy(m_client, &WsClient::disconnected);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_host->sendText(QStringLiteral("{not json"));

  // §7: malformed JSON logs and tears the connection down. The long reconnect
  // interval guarantees no retry can re-open it inside the wait window.
  QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() == 1 && !m_client->isConnected(), kWaitMs);
}

void TestProtocol::unknownActionFailsFast()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy disconnectedSpy(m_client, &WsClient::disconnected);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_host->sendText(QStringLiteral("{\"v\":1,\"action\":\"bogus\"}"));

  QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() == 1 && !m_client->isConnected(), kWaitMs);
}

void TestProtocol::badVersionFailsFast()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy disconnectedSpy(m_client, &WsClient::disconnected);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  m_host->sendText(QStringLiteral("{\"v\":2,\"action\":\"get_info\"}"));

  QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() == 1 && !m_client->isConnected(), kWaitMs);
}

void TestProtocol::exitOnDisconnectSemantics()
{
  QSignalSpy connectedSpy(m_client, &WsClient::connected);
  QSignalSpy disconnectedSpy(m_client, &WsClient::disconnected);

  m_client->connectToHost(m_host->url());
  QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, kWaitMs);

  // The host closes its end: what a host shutdown looks like to the app.
  m_host->closeClient();

  QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() == 1 && !m_client->isConnected(), kWaitMs);
}

QTEST_MAIN(TestProtocol)

#include "tst_protocol.moc"
