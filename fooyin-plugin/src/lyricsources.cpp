/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "lyricsources.h"

#include <unicode/unistr.h>

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <array>

namespace {

/// Tag lookup order matches fooyin's built-in lyrics plugin: synced sources
/// (LYRICS, SYNCEDLYRICS) win over unsynced ones. extraTag() is case-insensitive.
const QStringList kEmbeddedTagPriority{
    QStringLiteral("LYRICS"),
    QStringLiteral("SYNCEDLYRICS"),
    QStringLiteral("UNSYNCEDLYRICS"),
    QStringLiteral("UNSYNCED LYRICS"),
};

/// True when every byte sequence in `bytes` is well-formed UTF-8 (ASCII, or a
/// valid lead + continuation run). Rejects overlong encodings, UTF-16 surrogate
/// code points, and values above U+10FFFF.
bool isWellFormedUtf8(const QByteArray& bytes)
{
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    const qsizetype size = bytes.size();
    qsizetype i = 0;

    while (i < size) {
        const uchar lead = data[i];
        if (lead <= 0x7F) { // ASCII
            ++i;
            continue;
        }
        if (lead < 0xC2) { // stray continuation byte or invalid 0xC0/0xC1 lead
            return false;
        }

        int continuationCount = 0;
        uchar firstContMin = 0x80;
        uchar firstContMax = 0xBF;
        if (lead <= 0xDF) {
            continuationCount = 1;
        } else if (lead <= 0xEF) {
            continuationCount = 2;
            if (lead == 0xE0) {
                firstContMin = 0xA0; // no overlong 3-byte encoding
            } else if (lead == 0xED) {
                firstContMax = 0x9F; // no UTF-16 surrogate code points
            }
        } else if (lead <= 0xF4) {
            continuationCount = 3;
            if (lead == 0xF0) {
                firstContMin = 0x90; // no overlong 4-byte encoding
            } else if (lead == 0xF4) {
                firstContMax = 0x8F; // no code points above U+10FFFF
            }
        } else { // 0xF5-0xFF can never start a code point
            return false;
        }

        if (i + continuationCount >= size) {
            return false; // truncated sequence
        }
        for (int k = 1; k <= continuationCount; ++k) {
            const uchar cont = data[i + k];
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
            if (k == 1 && (cont < firstContMin || cont > firstContMax)) {
                return false;
            }
        }
        i += continuationCount + 1;
    }
    return true;
}

/// Decode UTF-16 bytes (after stripping the BOM) into a QString. Little-endian
/// is native; big-endian bytes are swapped first. A trailing odd byte is dropped.
QString decodeUtf16(const QByteArray& bytes, bool bigEndian)
{
    QByteArray nativeOrder;
    if (bigEndian) {
        nativeOrder.reserve(bytes.size());
        for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) {
            nativeOrder.append(bytes.at(i + 1));
            nativeOrder.append(bytes.at(i));
        }
    }
    const QByteArray& text = bigEndian ? nativeOrder : bytes;
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(text.constData()), text.size() / 2);
}

/// Decode raw file bytes into a trusted QString. Handles BOMs first, then
/// UTF-8, then legacy Chinese encodings via ICU. Returns empty on failure.
QString decodeLyricsBytes(const QByteArray& bytes, const QString& sourceForLog)
{
    if (bytes.isEmpty()) {
        return QString();
    }
    if (bytes.startsWith(QByteArrayLiteral("\xEF\xBB\xBF"))) {
        return QString::fromUtf8(bytes.constData() + 3, bytes.size() - 3);
    }
    if (bytes.startsWith(QByteArrayLiteral("\xFF\xFE"))) {
        return decodeUtf16(bytes.mid(2), /*bigEndian=*/false);
    }
    if (bytes.startsWith(QByteArrayLiteral("\xFE\xFF"))) {
        return decodeUtf16(bytes.mid(2), /*bigEndian=*/true);
    }
    if (isWellFormedUtf8(bytes)) {
        return QString::fromUtf8(bytes);
    }

    // Not UTF-8: try GB18030 (a superset of GBK), then BIG5. A failed
    // conversion yields a bogus string; a lossy one yields U+FFFD replacement
    // characters. Either way the bytes are unusable, so try the next encoding.
    const std::array<const char*, 2> encodings{"GB18030", "BIG5"};
    for (const char* encoding : encodings) {
        const icu::UnicodeString unicode(bytes.constData(), bytes.size(), encoding);
        if (!unicode.isBogus() && unicode.indexOf(0xFFFD) < 0) {
            return QString::fromUtf16(unicode.getBuffer(), unicode.length());
        }
    }

    qWarning() << "[LX Lyrics] failed to decode lyrics from" << sourceForLog
               << "as UTF-8, GB18030 or BIG5; returning empty lyrics";
    return QString();
}

/// First non-empty embedded tag, joined with '\n' for multi-value tags.
QString embeddedLyrics(const Fooyin::Track& track)
{
    for (const QString& tag : kEmbeddedTagPriority) {
        const QStringList values = track.extraTag(tag);
        if (!values.isEmpty()) {
            return values.join(QLatin1Char('\n'));
        }
    }
    // Description-suffixed fallback: TagLib maps a USLT frame with a non-empty
    // description (e.g. "eng" or a localized label, written by many taggers)
    // to "LYRICS:<description>" in the property map, so the exact lookups
    // above miss it. Scan the full extra-tag map once for the suffixed keys.
    const QStringList suffixedPrefixes{
        QStringLiteral("LYRICS:"),
        QStringLiteral("UNSYNCEDLYRICS:"),
        QStringLiteral("UNSYNCED LYRICS:"),
    };
    for (const QString& prefix : suffixedPrefixes) {
        const auto extraTags = track.extraTags();
        for (auto it = extraTags.cbegin(); it != extraTags.cend(); ++it) {
            if (it->first.startsWith(prefix) && !it->second.isEmpty()) {
                return it->second.join(QLatin1Char('\n'));
            }
        }
    }
    return QString();
}

/// Same-directory .lrc sidecar for the track file (e.g. "song.mp3" ->
/// "song.lrc"). Returns empty when absent or undecodable.
QString readSidecar(const Fooyin::Track& track)
{
    const QString filepath = track.filepath();
    if (filepath.isEmpty()) {
        return QString();
    }

    const QFileInfo trackInfo(filepath);
    const QString candidate = trackInfo.absolutePath() + QLatin1Char('/') + trackInfo.completeBaseName()
        + QStringLiteral(".lrc");
    if (!QFile::exists(candidate)) {
        return QString();
    }

    QFile file(candidate);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[LX Lyrics] cannot open local lyrics sidecar" << candidate
                   << "for track" << filepath;
        return QString();
    }

    return decodeLyricsBytes(file.readAll(), candidate);
}

/// Join the .lrc sidecar and the embedded-tag lyrics into ONE raw string.
/// Raw transport only — no LRC parsing happens here: the sidecar comes first
/// because it is the curated source and usually the superset. With the sidecar
/// first, its metadata tags ([offset:], [ti:] etc.) now take precedence when
/// both sources carry them (previously embedded's won). The app-side parser
/// merges duplicate timestamps into extended lyric lines and suppresses
/// exact-text duplicates, so translations reach the renderer instead of being
/// shadowed by a strict-subset embedded tag. Returns the non-empty source
/// when only one exists, empty when neither does.
QString combineLyricSources(const QString& sidecar, const QString& embedded)
{
    if (sidecar.isEmpty())
        return embedded;
    if (embedded.isEmpty())
        return sidecar;
    return sidecar + QLatin1Char('\n') + embedded;
}

} // namespace

namespace LxLyrics {

LyricsResult LyricSource::fetch(const Fooyin::Track& track)
{
    const QString embedded = embeddedLyrics(track);
    const QString sidecar = readSidecar(track);
    // The embedded tag and the sidecar are BOTH sent when present: many
    // sidecars are the curated superset (English lines + Chinese translations)
    // while the embedded tag holds only a strict subset, so sending just the
    // tag shadows the translations. No LRC parsing here — the app-side parser
    // merges the duplicate timestamps into extended lyric lines and suppresses
    // exact-text duplicates.
    const QString lrc = combineLyricSources(sidecar, embedded);
    return LyricsResult{lrc, QString(), QString(), QString()};
}

} // namespace LxLyrics
