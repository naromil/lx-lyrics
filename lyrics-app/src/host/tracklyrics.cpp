/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "host/tracklyrics.h"

#include <unicode/unistr.h>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <array>

namespace {

/// Embedded-tag lookup order, case-insensitive — the same order the plugin
/// uses (synced sources win over unsynced ones).
const QStringList kEmbeddedTagPriority{
  QStringLiteral("LYRICS"),
  QStringLiteral("SYNCEDLYRICS"),
  QStringLiteral("UNSYNCEDLYRICS"),
  QStringLiteral("UNSYNCED LYRICS"),
};

/// Description-suffixed fallback keys, in the plugin's order. TagLib maps an
/// ID3 USLT frame with a non-empty description (e.g. "eng") to
/// "LYRICS:<description>", so the exact lookups above miss it.
const QStringList kSuffixedPrefixes{
  QStringLiteral("LYRICS:"),
  QStringLiteral("UNSYNCEDLYRICS:"),
  QStringLiteral("UNSYNCED LYRICS:"),
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

/// One TagLib property value list as UTF-8 QStrings.
QStringList toQStringList(const TagLib::StringList& values)
{
  QStringList out;
  out.reserve(static_cast<qsizetype>(values.size()));
  for (const TagLib::String& value : values) {
    out.append(QString::fromUtf8(value.to8Bit(true).c_str()));
  }
  return out;
}

QString tagKey(const TagLib::String& key)
{
  return QString::fromUtf8(key.to8Bit(true).c_str());
}

/// First non-empty embedded tag, joined with '\n' for multi-value tags.
QString embeddedLyrics(const QString& localPath)
{
  // TagLib prints "Could not open file" to stderr for a missing path; the
  // protocol's answer for an unknown path is simply "no lyrics".
  if (localPath.isEmpty() || !QFile::exists(localPath)) {
    return QString();
  }

  const QByteArray utf8Path = localPath.toUtf8();
  const TagLib::FileRef ref(utf8Path.constData());
  if (ref.isNull() || ref.tag() == nullptr) {
    return QString();
  }

  const TagLib::PropertyMap properties = ref.tag()->properties();

  // Case-insensitive exact-key lookup (TagLib normalizes most keys to upper
  // case, but Vorbis comments come through as written).
  for (const QString& wanted : kEmbeddedTagPriority) {
    const auto it =
      std::find_if(properties.cbegin(), properties.cend(), [&wanted](const auto& property) {
        return tagKey(property.first).compare(wanted, Qt::CaseInsensitive) == 0;
      });
    if (it != properties.cend() && !it->second.isEmpty()) {
      return toQStringList(it->second).join(QLatin1Char('\n'));
    }
  }

  for (const QString& prefix : kSuffixedPrefixes) {
    const auto it =
      std::find_if(properties.cbegin(), properties.cend(), [&prefix](const auto& property) {
        return tagKey(property.first).startsWith(prefix, Qt::CaseInsensitive) &&
               !property.second.isEmpty();
      });
    if (it != properties.cend()) {
      return toQStringList(it->second).join(QLatin1Char('\n'));
    }
  }

  return QString();
}

/// Same-directory .lrc sidecar for the track file (e.g. "song.mp3" ->
/// "song.lrc"). Returns empty when absent or undecodable.
QString readSidecar(const QString& localPath)
{
  if (localPath.isEmpty()) {
    return QString();
  }

  const QFileInfo trackInfo(localPath);
  const QString candidate = trackInfo.absolutePath() + QLatin1Char('/') +
                            trackInfo.completeBaseName() + QStringLiteral(".lrc");
  if (!QFile::exists(candidate)) {
    return QString();
  }

  QFile file(candidate);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "tracklyrics: cannot open lyrics sidecar" << candidate << "for track"
               << localPath;
    return QString();
  }

  return decodeLyricsBytes(file.readAll());
}

} // namespace

QString decodeLyricsBytes(const QByteArray& bytes)
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

  qWarning() << "tracklyrics: failed to decode" << bytes.size()
             << "lyric bytes as UTF-8, GB18030 or BIG5; returning empty lyrics";
  return QString();
}

QString lyricsForTrackFile(const QString& localPath)
{
  QString embedded = embeddedLyrics(localPath);
  QString sidecar = readSidecar(localPath);

  if (sidecar.isEmpty()) {
    return embedded;
  }
  if (embedded.isEmpty()) {
    return sidecar;
  }
  // Both sources are kept: many sidecars are the curated superset (English
  // lines + translations) while the embedded tag holds only a strict subset.
  // The app-side parser merges the duplicate timestamps and suppresses
  // exact-text duplicates.
  return sidecar + QLatin1Char('\n') + embedded;
}
