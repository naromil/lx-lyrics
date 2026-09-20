/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QMetaType>
#include <QString>

// Typed representations of the host→app messages defined in docs/protocol.md
// §5. These are the ONLY way payloads leave FeedReader: everything downstream
// consumes typed values, never raw JSON (parse at the boundary).
//
// v2 dropped the v1 `id` and `line` fields: no transport computes line numbers
// any more (the app recomputes the active line from `played_time`), and nothing
// consumed the opaque track id. `path` is new — the host hands over the playing
// file and the APP resolves its lyrics (see host/tracklyrics.h).
struct TrackSnapshot {
  QString singer;
  QString name;
  QString album;
  // Local filesystem path of the playing file; empty when the host has none
  // (streams, CUE tracks).
  QString path;
  // Already resolved: the host's non-empty `lrc`, or else the lyrics read from
  // `path` (sidecar first, then embedded tags).
  QString lrc;
  QString tlrc;
  QString rlrc;
  QString lxlrc;
  bool isPlay = false;
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
  qint64 playedTimeMs = 0;
};

Q_DECLARE_METATYPE(TrackSnapshot)
Q_DECLARE_METATYPE(LyricSnapshot)
Q_DECLARE_METATYPE(PlaybackSnapshot)
