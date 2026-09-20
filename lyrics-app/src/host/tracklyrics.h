/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QByteArray>
#include <QString>

// The app's lyric acquisition (docs/protocol.md §5 set_info): the host sends
// the playing file's path and the app reads its lyrics itself. This is the one
// acquisition implementation every host relies on, so the chain below is the
// contract, not an implementation detail — two players handing over the same
// file must produce the same lyrics, and changing any step changes them:
//   decode chain (UTF-8 BOM → UTF-16 LE/BE BOM → strict in-file UTF-8 → ICU
//   GB18030 → ICU BIG5) → sidecar `<dir>/<completeBaseName>.lrc` next to the
//   file → embedded tag order (LYRICS → SYNCEDLYRICS → UNSYNCEDLYRICS →
//   "UNSYNCED LYRICS", then the description-suffixed LYRICS:/UNSYNCEDLYRICS:/
//   "UNSYNCED LYRICS:" keys, multi-value joined with '\n') → combine order
//   (sidecar + '\n' + embedded; the non-empty source alone when only one has
//   text; empty when neither does).

// Decodes raw lyric-file bytes into a trusted QString:
// UTF-8 BOM → UTF-16 LE/BE BOM → strict in-file UTF-8 validation →
// ICU GB18030 → ICU BIG5. Returns empty on total failure (with a qWarning).
QString decodeLyricsBytes(const QByteArray& bytes);

// Lyrics for a local track file: the `<dir>/<completeBaseName>.lrc` sidecar
// plus the file's embedded tag, joined with '\n' (sidecar first — it is the
// curated source and usually the superset; the app-side parser merges the
// duplicate timestamps). Empty when neither source yields anything.
QString lyricsForTrackFile(const QString& localPath);
