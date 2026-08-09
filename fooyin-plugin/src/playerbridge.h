/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include <core/player/playercontroller.h>
#include <core/track.h>

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

class HostServer;

class PlayerBridge : public QObject {
  Q_OBJECT

public:
  using LyricProvider = std::function<void(const Fooyin::Track&, QString& lrc, QString& tlrc,
                                           QString& rlrc, QString& lxlrc)>;

  explicit PlayerBridge(Fooyin::PlayerController* playerController, HostServer* host,
                        QObject* parent = nullptr);

  /// Seam for lyric acquisition (wired in task 3.4). Unset provider -> empty lyrics.
  void setLyricProvider(LyricProvider provider);
  void setPlaybackRate(double rate);

  void onClientConnected();
  void handleRequestInfo();
  void handleRequestStatus();
  void handleRequestAnalyserData();
  void stopPush();

signals:
  /// The app asked for one spectrum snapshot; task 3.5 pushes the 128-byte frame.
  void analyserDataRequested();

private slots:
  void onCurrentTrackChanged(const Fooyin::Track& track);
  void onCurrentTrackUpdated(const Fooyin::Track& track);
  void onPlayStateChanged(Fooyin::Player::PlayState state, Fooyin::Player::PlayState previous);
  void onPositionChanged(uint64_t ms);
  void onPositionMoved(uint64_t ms);

  // moc requires an explicit access specifier to terminate the private slots region above;
  // without it the methods below are parsed as slot declarations and moc fails.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
private:
  [[nodiscard]] Fooyin::Track currentTrack() const;
  [[nodiscard]] bool isPlaying() const;
  [[nodiscard]] qint64 playedTime() const;
  void currentLyrics(const Fooyin::Track& track, QString& lrc, QString& tlrc, QString& rlrc,
                     QString& lxlrc) const;
  [[nodiscard]] QVariantMap setInfoFields(const Fooyin::Track& track, bool isPlay,
                                          qint64 playedTime, const QString& lrc,
                                          const QString& tlrc, const QString& rlrc,
                                          const QString& lxlrc) const;
  [[nodiscard]] QVariantMap emptySetInfoFields() const;
  void pushTrack(const Fooyin::Track& track);

  Fooyin::PlayerController* m_playerController = nullptr;
  HostServer* m_host = nullptr;
  LyricProvider m_lyricProvider;
  Fooyin::Track m_currentTrack;
  double m_playbackRate = 1.0;
  bool m_pushing = false;
  qint64 m_lastStatusMs = -1;
};
