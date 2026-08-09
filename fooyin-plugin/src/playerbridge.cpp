/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "playerbridge.h"

#include "hostserver.h"

#include <QDebug>

namespace {
constexpr qint64 kStatusThrottleMs = 500;
}

PlayerBridge::PlayerBridge(Fooyin::PlayerController* playerController, HostServer* host,
                           QObject* parent)
  : QObject(parent)
  , m_playerController(playerController)
  , m_host(host)
{
  if (m_playerController == nullptr) {
    qWarning() << "[LX Lyrics] PlayerBridge created without a PlayerController; bridge inert";
    return;
  }

  connect(m_playerController, &Fooyin::PlayerController::currentTrackChanged, this,
          &PlayerBridge::onCurrentTrackChanged);
  connect(m_playerController, &Fooyin::PlayerController::currentTrackUpdated, this,
          &PlayerBridge::onCurrentTrackUpdated);
  connect(m_playerController, &Fooyin::PlayerController::playStateChanged, this,
          &PlayerBridge::onPlayStateChanged);
  connect(m_playerController, &Fooyin::PlayerController::positionChanged, this,
          &PlayerBridge::onPositionChanged);
  connect(m_playerController, &Fooyin::PlayerController::positionMoved, this,
          &PlayerBridge::onPositionMoved);
}

void PlayerBridge::setLyricProvider(LyricProvider provider)
{
  m_lyricProvider = std::move(provider);
}

void PlayerBridge::setPlaybackRate(double rate)
{
  m_playbackRate = rate;
}

void PlayerBridge::onClientConnected()
{
  m_pushing = true;
  handleRequestInfo();
}

void PlayerBridge::handleRequestInfo()
{
  if (!m_pushing) {
    return;
  }
  pushTrack(currentTrack());
}

void PlayerBridge::handleRequestStatus()
{
  if (!m_pushing) {
    return;
  }

  const bool playing = isPlaying();
  const qint64 time = playedTime();
  m_host->sendSetStatus(playing, -1, time);
  if (playing) {
    m_host->sendSetPlay(time);
  }
}

void PlayerBridge::handleRequestAnalyserData()
{
  if (!m_pushing) {
    return;
  }
  emit analyserDataRequested();
}

void PlayerBridge::stopPush()
{
  m_pushing = false;
}

void PlayerBridge::onCurrentTrackChanged(const Fooyin::Track& track)
{
  m_currentTrack = track;
  if (!m_pushing) {
    return;
  }

  qInfo() << "[LX Lyrics] track changed:" << track.artist() << "-" << track.title();
  pushTrack(track);
}

void PlayerBridge::onCurrentTrackUpdated(const Fooyin::Track& track)
{
  m_currentTrack = track;
  if (!m_pushing) {
    return;
  }
  pushTrack(track);
}

void PlayerBridge::onPlayStateChanged(Fooyin::Player::PlayState state,
                                      Fooyin::Player::PlayState previous)
{
  Q_UNUSED(previous)
  if (!m_pushing) {
    return;
  }

  switch (state) {
  case Fooyin::Player::PlayState::Playing:
    m_lastStatusMs = 0;
    m_host->sendSetStatus(true, -1, playedTime());
    m_host->sendSetPlay(playedTime());
    break;
  case Fooyin::Player::PlayState::Paused:
    m_host->sendSetStatus(false, -1, playedTime());
    break;
  case Fooyin::Player::PlayState::Stopped:
    m_host->sendSetStop();
    break;
  }
}

void PlayerBridge::onPositionChanged(uint64_t ms)
{
  if (!m_pushing) {
    return;
  }

  const qint64 position = static_cast<qint64>(ms);
  if (position - m_lastStatusMs < kStatusThrottleMs) {
    return;
  }

  m_lastStatusMs = position;
  m_host->sendSetStatus(isPlaying(), -1, position);
}

void PlayerBridge::onPositionMoved(uint64_t)
{
  if (!m_pushing) {
    return;
  }

  const qint64 time = playedTime();
  m_host->sendSetPlay(time);
  m_host->sendSetStatus(isPlaying(), -1, time);
}

Fooyin::Track PlayerBridge::currentTrack() const
{
  if (m_currentTrack.isValid()) {
    return m_currentTrack;
  }
  if (m_playerController != nullptr) {
    return m_playerController->currentTrack();
  }
  return Fooyin::Track{};
}

bool PlayerBridge::isPlaying() const
{
  return m_playerController != nullptr &&
         m_playerController->playState() == Fooyin::Player::PlayState::Playing;
}

qint64 PlayerBridge::playedTime() const
{
  return m_playerController != nullptr ? static_cast<qint64>(m_playerController->currentPosition())
                                       : 0;
}

void PlayerBridge::currentLyrics(const Fooyin::Track& track, QString& lrc, QString& tlrc,
                                 QString& rlrc, QString& lxlrc) const
{
  lrc = tlrc = rlrc = lxlrc = QString();
  if (m_lyricProvider) {
    m_lyricProvider(track, lrc, tlrc, rlrc, lxlrc);
  }
}

QVariantMap PlayerBridge::setInfoFields(const Fooyin::Track& track, bool isPlay, qint64 playedTime,
                                        const QString& lrc, const QString& tlrc,
                                        const QString& rlrc, const QString& lxlrc) const
{
  const QString id = track.uniqueFilepath().isEmpty() ? track.filepath() : track.uniqueFilepath();

  QVariantMap fields;
  fields.insert(QStringLiteral("id"), id);
  fields.insert(QStringLiteral("singer"), track.artist());
  fields.insert(QStringLiteral("name"), track.title());
  fields.insert(QStringLiteral("album"), track.album());
  fields.insert(QStringLiteral("lrc"), lrc);
  fields.insert(QStringLiteral("tlrc"), tlrc);
  fields.insert(QStringLiteral("rlrc"), rlrc);
  fields.insert(QStringLiteral("lxlrc"), lxlrc);
  fields.insert(QStringLiteral("isPlay"), isPlay);
  fields.insert(QStringLiteral("line"), -1);
  fields.insert(QStringLiteral("played_time"), playedTime);
  return fields;
}

QVariantMap PlayerBridge::emptySetInfoFields() const
{
  QVariantMap fields;
  fields.insert(QStringLiteral("id"), QVariant());
  fields.insert(QStringLiteral("singer"), QString());
  fields.insert(QStringLiteral("name"), QString());
  fields.insert(QStringLiteral("album"), QString());
  fields.insert(QStringLiteral("lrc"), QString());
  fields.insert(QStringLiteral("tlrc"), QString());
  fields.insert(QStringLiteral("rlrc"), QString());
  fields.insert(QStringLiteral("lxlrc"), QString());
  fields.insert(QStringLiteral("isPlay"), false);
  fields.insert(QStringLiteral("line"), -1);
  fields.insert(QStringLiteral("played_time"), 0);
  return fields;
}

void PlayerBridge::pushTrack(const Fooyin::Track& track)
{
  if (!track.isValid()) {
    m_host->sendSetInfo(emptySetInfoFields());
    return;
  }

  QString lrc;
  QString tlrc;
  QString rlrc;
  QString lxlrc;
  currentLyrics(track, lrc, tlrc, rlrc, lxlrc);

  const bool playing = isPlaying();
  const qint64 time = playedTime();

  m_host->sendSetInfo(setInfoFields(track, playing, time, lrc, tlrc, rlrc, lxlrc));
  m_host->sendSetLyric(lrc, tlrc, rlrc, lxlrc);
  if (playing) {
    m_host->sendSetPlay(time);
  }
}
