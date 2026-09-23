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

class FeedWriter;

namespace Fooyin {
class SettingsManager;
} // namespace Fooyin

/// Translates Fooyin playback events into protocol v2 feed frames
/// (docs/protocol.md §5). The plugin never acquires lyrics: the app reads them
/// from the file at `set_info.path`, so this bridge supplies playing context
/// (path, metadata, state, position) and nothing else.
class PlayerBridge : public QObject {
  Q_OBJECT

public:
  /// `settingsManager` is read for Fooyin's own shutdown state
  /// (Settings::Core::Shutdown): the player is stopped as part of quitting,
  /// and that teardown stop must not be forwarded (see onPlayStateChanged).
  explicit PlayerBridge(Fooyin::PlayerController* playerController, FeedWriter* writer,
                        Fooyin::SettingsManager* settingsManager = nullptr,
                        QObject* parent = nullptr);

  void setPlaybackRate(double rate);

  /// The child is up and `hello` has been written (FeedWriter::appStarted):
  /// start pushing and send the initial snapshot — v2 removed `get_info`, so
  /// the host pushes the snapshot right after spawn (pipe buffers make the
  /// spawn race-free).
  void onAppStarted();
  void handleRequestAnalyserData();
  void stopPush();

signals:
  /// The app asked for one spectrum snapshot; SpectrumSource replies with the
  /// 128-byte frame through the feed.
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
  void pushTrack(const Fooyin::Track& track);

  Fooyin::PlayerController* m_playerController = nullptr;
  FeedWriter* m_writer = nullptr;
  /// Fooyin's settings, read only to tell "the user stopped playback" apart
  /// from "Fooyin is quitting and stopped playback on its way out".
  Fooyin::SettingsManager* m_settingsManager = nullptr;
  Fooyin::Track m_currentTrack;
  double m_playbackRate = 1.0;
  bool m_pushing = false;
  qint64 m_lastStatusMs = -1;
};
