/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "engine/lyricselector.h"

#include <QByteArray>
#include <QRegularExpression>

#include <algorithm>

namespace {

// JS tagRxp = /(?:^|\n\s*)\[awlrc:([^\]]+)]/i from music.ts parseLyric. The JS
// regex has NO /g flag, so String.prototype.replace replaces only the FIRST
// match, whereas QString::replace(QRegularExpression) replaces ALL matches.
// extractAwlrc therefore finds the first match manually and removes exactly its
// range (see the comment there).
const QRegularExpression kAwlrcTagRxp(QStringLiteral("(?:^|\\n\\s*)\\[awlrc:([^\\]]+)]"),
                                      QRegularExpression::CaseInsensitiveOption);
// JS lrcRxp = /^(lrc|awlrc|tlrc|rlrc):([^,]+)$/i — one key:base64 part.
const QRegularExpression kPartRxp(QStringLiteral("^(lrc|awlrc|tlrc|rlrc):([^,]+)$"),
                                  QRegularExpression::CaseInsensitiveOption);
// JS verifyAwlrc = /(?:^|\s*)\[\d+:\d+(?:\.\d+)]<\d+,\d+>.+$/m — a word-tagged
// ("karaoke") line, so a bare "<0,9999>Hello" is rejected.
const QRegularExpression
  kAwlrcVerifyRxp(QStringLiteral("(?:^|\\s*)\\[\\d+:\\d+(?:\\.\\d+)]<\\d+,\\d+>.+$"),
                  QRegularExpression::MultilineOption);
// JS verifylrc = /(?:^|\s*)\[\d+:\d+(?:\.\d+)].+$/m — a plain timed line.
const QRegularExpression kLrcVerifyRxp(QStringLiteral("(?:^|\\s*)\\[\\d+:\\d+(?:\\.\\d+)].+$"),
                                       QRegularExpression::MultilineOption);

// JS parse() from music.ts: split the container content on ',', decode each
// key:base64 part and keep only parts that pass their verify regex. Parts that
// fail verify are silently dropped, exactly like the JS
// `if (target.verify(data))` guard.
AwlrcParts parseAwlrcContent(const QString& content)
{
  AwlrcParts parts;
  const QStringList partsList = content.trimmed().split(QLatin1Char(','));
  for (const QString& rawPart : partsList) {
    const QRegularExpressionMatch partMatch = kPartRxp.match(rawPart.trimmed());
    if (!partMatch.hasMatch())
      continue; // JS: if (!result) continue
    const QString key = partMatch.captured(1).toLower();
    // JS: Buffer.from(result[2], 'base64').toString('utf-8').trim()
    const QString decoded =
      QString::fromUtf8(QByteArray::fromBase64(partMatch.captured(2).toLatin1())).trimmed();
    if (key == QLatin1String("lrc") && kLrcVerifyRxp.match(decoded).hasMatch())
      parts.lyric = decoded;
    else if (key == QLatin1String("tlrc") && kLrcVerifyRxp.match(decoded).hasMatch())
      parts.tlyric = decoded;
    else if (key == QLatin1String("rlrc") && kLrcVerifyRxp.match(decoded).hasMatch())
      parts.rlyric = decoded;
    else if (key == QLatin1String("awlrc") && kAwlrcVerifyRxp.match(decoded).hasMatch())
      parts.lxlyric = decoded;
  }
  // found: the container tag existed (the caller only invokes this for a
  // matched tag) and at least one part was accepted.
  parts.found = !parts.lyric.isEmpty() || !parts.tlyric.isEmpty() || !parts.rlyric.isEmpty() ||
                !parts.lxlyric.isEmpty();
  return parts;
}

} // namespace

AwlrcParts LyricSelector::extractAwlrc(const QString& lrc, QString* strippedLrc)
{
  const QRegularExpressionMatch tagMatch = kAwlrcTagRxp.match(lrc);

  // JS lrc.replace(tagRxp, cb) — no /g flag, so exactly ONE removal. The match
  // range includes the leading newline/whitespace when the tag sits on its own
  // line, so removing capturedStart()..capturedLength() and trimming mirrors
  // the JS single replacement + trim.
  QString stripped = lrc.trimmed();
  AwlrcParts parts;
  if (tagMatch.hasMatch()) {
    stripped = lrc;
    stripped.remove(tagMatch.capturedStart(), tagMatch.capturedLength());
    stripped = stripped.trimmed();
    parts = parseAwlrcContent(tagMatch.captured(1));
  }

  if (strippedLrc)
    *strippedLrc = stripped;
  return parts;
}

void LyricSelector::setPlayLxlrc(bool on)
{
  m_playLxlrc = on;
}

void LyricSelector::setIsShowLyricTranslation(bool on)
{
  m_showTranslation = on;
}

void LyricSelector::setIsShowLyricRoma(bool on)
{
  m_showRoma = on;
}

void LyricSelector::setIsSwapLyricTranslationAndRoma(bool on)
{
  m_swap = on;
}

void LyricSelector::setLyrics(const QString& lrc, const QString& tlyric, const QString& rlyric,
                              const QString& lxlyric)
{
  QString stripped;
  const AwlrcParts parts = extractAwlrc(lrc, &stripped);

  // JS parseLyric: return { lyric, ...parsedInfo } — the stripped lrc replaces
  // the raw field, then accepted parts override their targets (lrc -> lyric,
  // tlrc -> tlyric, rlrc -> rlyric, awlrc -> lxlyric).
  m_lyric = stripped;
  m_tlyric = tlyric;
  m_rlyric = rlyric;
  m_lxlyric = lxlyric;
  if (!parts.lyric.isEmpty())
    m_lyric = parts.lyric;
  if (!parts.tlyric.isEmpty())
    m_tlyric = parts.tlyric;
  if (!parts.rlyric.isEmpty())
    m_rlyric = parts.rlyric;
  if (!parts.lxlyric.isEmpty())
    m_lxlyric = parts.lxlyric;
}

QString LyricSelector::selectedLyric() const
{
  // JS setLyric(): setting['player.isPlayLxlrc'] && lyrics.lxlyric
  //                ? lyrics.lxlyric : lyrics.lyric
  if (m_playLxlrc && !m_lxlyric.isEmpty())
    return m_lxlyric;
  return m_lyric;
}

QStringList LyricSelector::extendedLyrics() const
{
  // JS setLyric(): push rlyric first when showing roma, then tlyric when
  // showing translation; reverse when the two are swapped. Empty strings are
  // skipped (JS truthiness).
  QStringList result;
  if (m_showRoma && !m_rlyric.isEmpty())
    result.append(m_rlyric);
  if (m_showTranslation && !m_tlyric.isEmpty())
    result.append(m_tlyric);
  if (m_swap)
    std::reverse(result.begin(), result.end());
  return result;
}

bool LyricSelector::hasLyrics() const
{
  return !selectedLyric().isEmpty();
}
