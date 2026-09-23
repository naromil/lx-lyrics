/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "playerbridge.h"

#include "feedwriter.h"

#include <core/coresettings.h>
#include <utils/settings/settingsmanager.h>

#include <QDebug>

namespace {
constexpr qint64 kStatusThrottleMs = 500;

/// protocol v2 §5 `set_info`: playing context only. `id` and `line` were
/// removed in v2 (no host computes line numbers, no consumer for `id`) and no
/// lyric field is sent — the app reads the lyrics itself from the file at
/// `path` (sidecar, then embedded tags).
QVariantMap setInfoFields(const Fooyin::Track& track, bool isPlay, qint64 playedTime)
{
  QVariantMap fields;
  fields.insert(QStringLiteral("path"), track.filepath());
  fields.insert(QStringLiteral("singer"), track.artist());
  fields.insert(QStringLiteral("name"), track.title());
  fields.insert(QStringLiteral("album"), track.album());
  fields.insert(QStringLiteral("isPlay"), isPlay);
  fields.insert(QStringLiteral("played_time"), playedTime);
  return fields;
}

/// The same snapshot for "nothing is playing": `path` is "" when unknown.
QVariantMap emptySetInfoFields()
{
  QVariantMap fields;
  fields.insert(QStringLiteral("path"), QString());
  fields.insert(QStringLiteral("singer"), QString());
  fields.insert(QStringLiteral("name"), QString());
  fields.insert(QStringLiteral("album"), QString());
  fields.insert(QStringLiteral("isPlay"), false);
  fields.insert(QStringLiteral("played_time"), 0);
  return fields;
}
} // namespace

PlayerBridge::PlayerBridge(Fooyin::PlayerController* playerController, FeedWriter* writer,
                           Fooyin::SettingsManager* settingsManager, QObject* parent)
  : QObject(parent)
  , m_playerController(playerController)
  , m_writer(writer)
  , m_settingsManager(settingsManager)
{
  if (m_playerController == nullptr) {
    qWarning() << "[LX Lyrics] PlayerBridge created without a PlayerController; bridge inert";
    return;
  }
  if (m_writer == nullptr) {
    qWarning() << "[LX Lyrics] PlayerBridge created without a FeedWriter; bridge inert";
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

void PlayerBridge::setPlaybackRate(double rate)
{
  m_playbackRate = rate;
}

void PlayerBridge::onAppStarted()
{
  m_pushing = true;
  pushTrack(currentTrack());
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
    m_writer->sendSetStatus(true, playedTime());
    m_writer->sendSetPlay(playedTime());
    break;
  case Fooyin::Player::PlayState::Paused:
    m_writer->sendSetStatus(false, playedTime());
    break;
  case Fooyin::Player::PlayState::Stopped:
    // Fooyin stops playback as part of quitting (MainWindow::exit ->
    // Settings::Core::Shutdown -> PlayerController::stop(), all synchronous),
    // so this Stopped can be that teardown stop rather than user intent.
    // Forwarding it would clear the app's lyric: the window would show "No
    // lyrics" for as long as the engine takes to drain, right before the
    // session ends. The app keeps its last frame instead and exits on the
    // stdin EOF that ends the session. A real stop (the user pressing Stop)
    // still goes through - the setting is only true while Fooyin is quitting.
    if (m_settingsManager != nullptr &&
        m_settingsManager->value<Fooyin::Settings::Core::Shutdown>()) {
      return;
    }
    m_writer->sendSetStop();
    break;
  }
}

void PlayerBridge::onPositionChanged(uint64_t ms)
{
  if (!m_pushing) {
    return;
  }

  const qint64 position = static_cast<qint64>(ms);
  // Throttle only forwards: a position below the last one sent is a backwards
  // seek, and the app has just been re-anchored, so that update must go now.
  if (position >= m_lastStatusMs && position - m_lastStatusMs < kStatusThrottleMs) {
    return;
  }

  m_lastStatusMs = position;
  m_writer->sendSetStatus(isPlaying(), position);
}

void PlayerBridge::onPositionMoved(uint64_t)
{
  if (!m_pushing) {
    return;
  }

  const qint64 time = playedTime();
  m_writer->sendSetPlay(time);
  m_writer->sendSetStatus(isPlaying(), time);
  // The app's timer restarts at the seek target: re-anchor the throttle there,
  // so the next periodic update is due 500 ms of playback from now instead of
  // being suppressed until playback passes the pre-seek position.
  m_lastStatusMs = time;
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

void PlayerBridge::pushTrack(const Fooyin::Track& track)
{
  if (m_writer == nullptr) {
    return;
  }

  if (!track.isValid()) {
    m_writer->sendSetInfo(emptySetInfoFields());
    return;
  }

  const bool playing = isPlaying();
  const qint64 time = playedTime();

  m_writer->sendSetInfo(setInfoFields(track, playing, time));
  if (playing) {
    m_writer->sendSetPlay(time);
  }
}
