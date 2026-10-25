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
#include <QRegularExpression>
#include <QStringList>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace {

/// The reference's local-sidecar cap: a `.lrc`/`.krc` at or below 10 MiB is
/// read, anything larger is skipped before readAll() (a warning, no sidecar).
constexpr qint64 kMaxSidecarBytes = 10 * 1024 * 1024;

/// Kugou KRC obfuscation key, verbatim from
/// references/src/common/utils/lyricUtils/kg.js (`enc_key`). The file layout
/// is a 4-byte header, then the zlib stream XORed byte-wise with this key.
constexpr std::array<std::uint8_t, 16> kKrcXorKey{
  0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47, 0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69,
};

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
      if ((static_cast<unsigned int>(cont) & 0xC0U) != 0x80U) {
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

/// Inflates a zlib stream into `plain`; false when the stream is malformed or
/// truncated. The output grows with the actual stream (no sidecar-sized
/// buffer guess).
bool inflateZlib(const QByteArray& compressed, QByteArray* plain)
{
  z_stream stream{};
  if (inflateInit(&stream) != Z_OK) {
    return false;
  }

  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
  stream.avail_in = static_cast<uInt>(compressed.size());

  std::array<char, 64 * 1024> buffer{};
  int status = Z_OK;
  while (status == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    status = inflate(&stream, Z_NO_FLUSH);
    plain->append(buffer.data(), static_cast<qsizetype>(buffer.size() - stream.avail_out));
  }
  inflateEnd(&stream);
  return status == Z_STREAM_END;
}

/// One pass over the reference's decodeName map (`&amp;` -> `&`, ...). Entities
/// are replaced without rescanning the output, exactly like Node's single
/// replace() pass, so "&amp;lt;" becomes "&lt;" rather than "<".
QString decodeKrcEntities(const QString& text)
{
  static const QRegularExpression entityExp(
    QStringLiteral(R"(&(?:amp|lt|gt|quot|apos|#039|nbsp);)"));
  QString out;
  out.reserve(text.size());

  qsizetype copied = 0;
  auto matches = entityExp.globalMatch(text);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    out += text.mid(copied, match.capturedStart() - copied);
    const QString entity = match.captured();
    if (entity == QStringLiteral("&nbsp;")) {
      out += QLatin1Char(' ');
    } else if (entity == QStringLiteral("&amp;")) {
      out += QLatin1Char('&');
    } else if (entity == QStringLiteral("&lt;")) {
      out += QLatin1Char('<');
    } else if (entity == QStringLiteral("&gt;")) {
      out += QLatin1Char('>');
    } else if (entity == QStringLiteral("&quot;")) {
      out += QLatin1Char('"');
    } else {
      out += QLatin1Char('\''); // &apos; / &#039;
    }
    copied = match.capturedEnd();
  }
  out += text.mid(copied);
  return out;
}

/// KRC `[start,duration]` fields -> LRC `[mm:ss.mmm]`. Minutes and seconds are
/// zero-padded to two digits like the reference. DELIBERATE DEVIATION from
/// kg.js: its milliseconds are unpadded, but the LRC parser reads fractional
/// digits as a decimal fraction (1 digit x100, 2 x10, 3 x1), so the
/// reference's `[00:01.5]` for 1005 ms would mean 1500 ms. Padding to exactly
/// three digits keeps the original KRC start: 1005 ms -> `[00:01.005]`.
QString krcTimestampsToLrc(const QString& text)
{
  static const QRegularExpression timeExp(QStringLiteral(R"(\[(\d+),\d+\])"));
  QString out;
  out.reserve(text.size());

  qsizetype copied = 0;
  auto matches = timeExp.globalMatch(text);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    out += text.mid(copied, match.capturedStart() - copied);
    bool ok = false;
    const qlonglong milliseconds = match.captured(1).toLongLong(&ok);
    if (!ok) {
      out += match.captured(); // Unrepresentable: keep the field as written.
    } else {
      out += QStringLiteral("[%1:%2.%3]")
               .arg(milliseconds / 60000, 2, 10, QLatin1Char('0'))
               .arg((milliseconds / 1000) % 60, 2, 10, QLatin1Char('0'))
               .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
    }
    copied = match.capturedEnd();
  }
  out += text.mid(copied);
  return out;
}

/// Removes KRC `<start,duration>` word tags: first normalize the reference's
/// three-field form `<start,duration,?>` to `<start,duration>`, then delete
/// every remaining two-field tag.
QString stripKrcWordTags(const QString& text)
{
  QString out = text;
  static const QRegularExpression threeFieldExp(QStringLiteral(R"(<(\d+,\d+),\d+>)"));
  out.replace(threeFieldExp, QStringLiteral("<\\1>"));
  static const QRegularExpression wordTagExp(QStringLiteral(R"(<\d+,\d+>)"));
  out.remove(wordTagExp);
  return out;
}

/// Reference parseLyric(): drop CRs, the `[id:$...]` header and the
/// `[language:...]` metadata line, convert timestamps, strip word tags, then
/// resolve HTML entities. Text and line order are otherwise preserved.
QString parseKrcText(const QString& raw)
{
  QString text = raw;
  text.remove(QLatin1Char('\r'));

  static const QRegularExpression idHeaderExp(QStringLiteral(R"(^.*\[id:\$\w+\]\n)"));
  text.remove(idHeaderExp);

  // The language line carries a base64 translation JSON block that feeds the
  // reference's separate tlyric/rlyric strings; it is never part of `lyric`,
  // which is the text this acquisition path returns.
  static const QRegularExpression languageExp(QStringLiteral(R"(\[language:[\w=\\/+]\]\n)"));
  text.remove(languageExp);

  text = krcTimestampsToLrc(text);
  text = stripKrcWordTags(text);
  return decodeKrcEntities(text);
}

/// Raw Kugou .krc sidecar: 4-byte header ("krc1"), XOR-obfuscated zlib stream,
/// UTF-8 text. Malformed/truncated/corrupt input returns empty and logs
/// exactly one qWarning; it never reads past the compressed payload.
QString decodeKrcBytes(const QByteArray& bytes, const QString& path)
{
  if (bytes.size() <= 4) {
    qWarning() << "tracklyrics: KRC sidecar is truncated" << path;
    return QString();
  }

  QByteArray compressed = bytes.mid(4);
  for (qsizetype i = 0; i < compressed.size(); ++i) {
    compressed[i] =
      static_cast<char>(static_cast<std::uint8_t>(compressed.at(i)) ^
                        kKrcXorKey.at(static_cast<std::size_t>(i) % kKrcXorKey.size()));
  }

  QByteArray plain;
  if (!inflateZlib(compressed, &plain)) {
    qWarning() << "tracklyrics: cannot inflate KRC sidecar" << path;
    return QString();
  }

  return parseKrcText(QString::fromUtf8(plain));
}

/// Reads a sidecar only when present and within the 10 MiB cap; absent files
/// are silent, an oversized or unreadable one warns and yields no bytes (the
/// caller may then try the next sidecar). Never calls readAll() past the cap.
std::optional<QByteArray> readCappedSidecar(const QString& path)
{
  const QFileInfo info(path);
  if (!info.exists()) {
    return std::nullopt;
  }
  if (info.size() > kMaxSidecarBytes) {
    qWarning() << "tracklyrics: lyrics sidecar is larger than 10 MiB; ignoring" << path;
    return std::nullopt;
  }

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "tracklyrics: cannot open lyrics sidecar" << path;
    return std::nullopt;
  }
  return file.readAll();
}

/// Sidecar chain for the track file (e.g. "song.mp3"): `song.lrc` first, then
/// `song.krc` when the .lrc is absent, empty, or undecodable. The .lrc wins
/// whenever it yields text. Both are capped at 10 MiB. Empty when neither
/// exists or decodes.
QString readSidecar(const QString& localPath)
{
  if (localPath.isEmpty()) {
    return QString();
  }

  const QFileInfo trackInfo(localPath);
  const QString base = trackInfo.absolutePath() + QLatin1Char('/') + trackInfo.completeBaseName();

  if (const std::optional<QByteArray> bytes = readCappedSidecar(base + QStringLiteral(".lrc"))) {
    const QString text = decodeLyricsBytes(*bytes);
    if (!text.isEmpty()) {
      return text;
    }
  }

  const QString krcPath = base + QStringLiteral(".krc");
  if (const std::optional<QByteArray> bytes = readCappedSidecar(krcPath)) {
    return decodeKrcBytes(*bytes, krcPath);
  }
  return QString();
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
