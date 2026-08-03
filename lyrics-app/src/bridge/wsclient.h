/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QAbstractSocket>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

// Typed representations of the host→app messages defined in docs/protocol.md
// §5. These are the ONLY way payloads leave WsClient: everything downstream
// consumes typed values, never raw JSON (parse at the boundary).
struct TrackSnapshot {
    QString id;
    QString singer;
    QString name;
    QString album;
    QString lrc;
    QString tlrc;
    QString rlrc;
    QString lxlrc;
    bool isPlay = false;
    qint64 line = -1;
    qint64 playedTimeMs = 0;
};

struct LyricSnapshot {
    QString lrc;
    QString tlrc;
    QString rlrc;
    QString lxlrc;
};

struct PlaybackSnapshot {
    bool isPlay = false;
    qint64 line = -1;
    qint64 playedTimeMs = 0;
};

Q_DECLARE_METATYPE(TrackSnapshot)
Q_DECLARE_METATYPE(LyricSnapshot)
Q_DECLARE_METATYPE(PlaybackSnapshot)

// WebSocket client for the desktop-lyrics bridge (docs/protocol.md).
//
// Transport only: connects to the host, sends the app→host request frames
// (§4), and parses every host→app frame strictly at the socket boundary (§5).
// Text frames are JSON messages, dispatched to typed signals; the single
// binary frame type is the 128-byte analyser payload. Per §7 the client fails
// fast: malformed JSON, a missing/unsupported `v`, or an unknown `action`
// logs the error and tears the connection down without ever emitting
// half-parsed data.
class WsClient : public QObject {
    Q_OBJECT

public:
    static constexpr int kDefaultReconnectIntervalMs = 2000;

    explicit WsClient(int reconnectIntervalMs = kDefaultReconnectIntervalMs,
                      QObject* parent = nullptr);

    // Starts connecting (or reconnecting, when the host respawns per §2).
    // Reconnect retries every reconnectIntervalMs while a connection is
    // expected but not established.
    void connectToHost(const QUrl& url);
    // Stops reconnecting and closes the current connection.
    void disconnectFromHost();
    bool isConnected() const;

    // App→host requests (§4). No-ops while not connected (mirrors the
    // reference sendDesktopLyricInfo, which drops messages without a port).
    void sendGetInfo();
    void sendGetStatus();
    void sendGetAnalyserData();

signals:
    void connected();
    void disconnected();

    void infoReceived(const TrackSnapshot& info);
    void lyricReceived(const LyricSnapshot& lyric);
    void statusReceived(const PlaybackSnapshot& status);
    void offsetReceived(qint64 tempOffset);
    void playbackRateReceived(double rate);
    void playReceived(qint64 timeMs);
    void pauseReceived();
    void stopReceived();
    // Exactly 128 bytes of log-scaled spectrum magnitudes.
    void analyserDataReceived(const QByteArray& data);

private:
    void openSocket();
    void retryConnect();
    void sendRequest(const QString& action);
    void onSocketStateChanged(QAbstractSocket::SocketState state);
    void failProtocol(const QString& reason);

    void dispatchIncomingText(const QString& frame);
    void dispatchIncomingBinary(const QByteArray& frame);

    QWebSocket m_socket;
    QTimer m_reconnectTimer;
    QUrl m_url;
    int m_reconnectIntervalMs = kDefaultReconnectIntervalMs;
    bool m_connectionExpected = false;
};
