/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include <core/track.h>

#include <QString>

namespace LxLyrics {

/// The four opaque lyric strings the protocol §5 carries per track. Only lrc is
/// populated in v1 (embedded tags / local .lrc files); the app parses it.
struct LyricsResult
{
    QString lrc;   // synced LRC or unsynced lyrics text (raw UTF-8 semantics)
    QString tlrc;  // translation — always empty in v1 (no source)
    QString rlrc;  // romaji — always empty in v1 (no source)
    QString lxlrc; // empty in v1 (no word-timed source; raw tags pass as lrc)
};

class LyricSource
{
public:
    /// Raw lyrics for a track. Embedded tags win over the sidecar; a missing
    /// or undecodable source yields an empty lrc (never a half-decoded string).
    static LyricsResult fetch(const Fooyin::Track& track);
};

} // namespace LxLyrics
