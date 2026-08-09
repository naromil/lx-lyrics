/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "engine/wordparser.h"

#include <QRegularExpression>

namespace {

// JS fontSplitRxp: /(?=<\d+,\d+>).*?/g.
//
// QString::split() cannot be used directly: Qt re-runs the pattern with a
// non-empty constraint after each zero-width match, so it yields spurious
// segments that consume "<" (verified against Qt 6.11). JS String.prototype.
// split() instead produces one segment per tag position, extending to the next
// tag (or end of text), with the text before the first tag as its own segment.
// The zero-width matches of the pattern mark exactly those tag boundaries, so
// the segments are rebuilt from them.
const QRegularExpression kFontSplitRxp(QStringLiteral("(?=<\\d+,\\d+>).*?"));
// JS timeRxp: /<(\d+),(\d+)>/ — non-global, so only the first tag per segment.
const QRegularExpression kTimeRxp(QStringLiteral("<(\\d+),(\\d+)>"));
// JS index.js line 117: /^<\d+,\d+>/ — line-mode decision for the first line.
const QRegularExpression kTimeTagStartRxp(QStringLiteral("^<\\d+,\\d+>"));

QStringList splitIntoSegments(const QString& text)
{
  QVector<qsizetype> tagPositions;
  auto matches = kFontSplitRxp.globalMatch(text);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    if (match.capturedStart() == match.capturedEnd())
      tagPositions.append(match.capturedStart()); // Zero-width: a tag boundary.
  }

  QStringList segments;
  if (tagPositions.isEmpty()) {
    segments << text;
    return segments;
  }

  // JS drops the empty segment that would precede a tag at position 0.
  if (tagPositions.first() > 0)
    segments << text.left(tagPositions.first());
  for (int i = 0; i < tagPositions.size(); ++i) {
    const qsizetype start = tagPositions.at(i);
    const qsizetype end = (i + 1 < tagPositions.size()) ? tagPositions.at(i + 1) : text.size();
    segments << text.mid(start, end - start);
  }
  return segments;
}

} // namespace

WordParser::Result WordParser::parse(const QString& lineText)
{
  Result result;
  for (const QString& segment : splitIntoSegments(lineText)) {
    const QRegularExpressionMatch tagMatch = kTimeRxp.match(segment);
    if (!tagMatch.hasMatch()) {
      // JS: if (!timeRxp.test(font)) return this._handleLineParse().
      // Any segment without a tag — including plain text before the first
      // tag — flips the whole line to line mode with the full text.
      result.isLineMode = true;
      result.words = {LyricWord{lineText, 0, 0}};
      return result;
    }
    // JS: font.replace(timeRxp, '') removes the first tag; RegExp.$1 / $2
    // carry startMs / durationMs.
    // Note: karaoke \d+ durations are unbounded (LRC caps time fields at 3
    // digits); qint64 overflow on absurd input is accepted as unrealistic.
    QString wordText = segment;
    wordText.remove(tagMatch.capturedStart(), tagMatch.capturedLength());
    result.words.append(
      LyricWord{wordText, tagMatch.captured(1).toLongLong(), tagMatch.captured(2).toLongLong()});
  }
  return result;
}

bool WordParser::hasTimeTags(const QString& text)
{
  return kTimeTagStartRxp.match(text).hasMatch();
}
