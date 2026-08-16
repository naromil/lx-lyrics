/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "engine/lrcparser.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <limits>

namespace {

// JS: timeFieldExp = /^(?:\[[\d:.]+\])+/g — anchored, so it only consumes the
// leading block of time tags. DELIBERATE DEVIATION, a documented bug fix over
// the JS reference: in line-player.js the /g flag makes the regex stateful via
// exec()'s lastIndex, so consecutive timed lines alternate match/drop (every
// other line fails to match because ^ cannot match once lastIndex advanced).
// The C++ port parses EVERY line; the decision is recorded here.
const QRegularExpression kTimeFieldExp(QStringLiteral("^(?:\\[[\\d:.]+\\])+"));
// Lines like "[00:00.-1]..." carry a time-shaped field that kTimeFieldExp
// cannot consume (the '-' is outside [\d:.]). Such a field is a broken
// timestamp, NOT a tag: the text after it is kept as a static lyric line.
// DELIBERATE DEVIATION from line-player.js, which drops these lines.
// Matches ONE broken field at a time so valid fields following it are
// re-parsed: "[00:00.-1][00:05.00]text" peels the broken field and still
// yields the timed 5 s line; an all-broken run still terminates as static.
const QRegularExpression kInvalidTimeFieldExp(QStringLiteral("^(?:\\[[\\d:.\\-]+\\])"));
// JS: timeExp = /\d{1,3}(:\d{1,3}){0,2}(?:\.\d{1,3})/g — every timestamp
// inside the time field.
const QRegularExpression kTimeExp(QStringLiteral("\\d{1,3}(:\\d{1,3}){0,2}(?:\\.\\d{1,3})"));

// JS t_rxp_1 /^0+(\d+)/, t_rxp_2 /:0+(\d+)/g, t_rxp_3 /\.0+(\d+)/.
// Note t_rxp_3 only strips a zero run that is followed by more digits: ".050"
// becomes ".50" but ".50" is unchanged. Qt's replace() honours the \1
// backreference exactly like JS "$1" but replaces ALL occurrences, whereas the
// JS t_rxp_3 (no /g) replaces only the first — unreachable here because
// kTimeExp allows at most one '.' per timestamp.
const QRegularExpression kLeadingZeroRxp(QStringLiteral("^0+(\\d+)"));
const QRegularExpression kColonZeroRxp(QStringLiteral(":0+(\\d+)"));
const QRegularExpression kDotZeroRxp(QStringLiteral("\\.0+(\\d+)"));

struct TagField {
  const char* code;
  QString LrcTag::* field;
};

// JS tagRegMap: { title: 'ti', artist: 'ar', album: 'al', offset: 'offset', by: 'by' }.
constexpr std::array kTagFields = {TagField{"ti", &LrcTag::title}, TagField{"ar", &LrcTag::artist},
                                   TagField{"al", &LrcTag::album}, TagField{"by", &LrcTag::by}};

QString formatTimeLabel(const QString& label)
{
  // The stripped label is the map key: equivalent timestamps such as
  // "[00:01.00]" and "[00:01.0]" both key to "0:1.0" and therefore merge.
  QString stripped = label;
  stripped.replace(kLeadingZeroRxp, QStringLiteral("\\1"));
  stripped.replace(kColonZeroRxp, QStringLiteral(":\\1"));
  stripped.replace(kDotZeroRxp, QStringLiteral(".\\1"));
  return stripped;
}

// JS parseInt semantics for the [offset:] tag, minus the radix inference:
// optional leading whitespace, an optional sign, then the LONGEST LEADING RUN
// of ASCII decimal digits; everything after the first non-decimal character
// is ignored, so "500ms" -> 500, "1e3" -> 1, "1500.5" -> 1500, " 700 " -> 700,
// "-500" -> -500. A value with no leading digits ("abc", "", "0x10") yields 0
// — parseInt would read "0x10" as hex 16, a DELIBERATE DEVIATION pinned by
// the lrcParserHexOffsetDefaultsZero test. The digit scan is ASCII-only like
// JS parseInt: a Unicode decimal digit (e.g. full-width '１') stops the scan
// like any other non-ASCII character and never contributes a value. Saturates
// at SIGN-SPECIFIC qint64 bounds instead of overflowing on pathological digit
// runs: positive overflow clamps to qint64::max(), negative overflow clamps
// to qint64::min(), and the exact |qint64::min()| magnitude (2^63) is
// representable.
qint64 parseLeadingDecimalInt(const QString& text)
{
  int i = 0;
  while (i < text.size() && text.at(i).isSpace())
    ++i; // JS parseInt skips leading whitespace.

  bool negative = false;
  if (i < text.size() && (text.at(i) == QLatin1Char('+') || text.at(i) == QLatin1Char('-'))) {
    negative = text.at(i) == QLatin1Char('-');
    ++i;
  }

  // The magnitude is accumulated unsigned: |qint64::min()| is 2^63, one more
  // than qint64::max(), so it only fits in an unsigned type — and negating an
  // overflowing SIGNED magnitude would be undefined arithmetic. The guard
  // below only multiplies when value * 10 + digit stays within the limit, so
  // the unsigned math never overflows either.
  const int digitsStart = i;
  quint64 value = 0;
  const quint64 kMagnitudeLimit =
    negative ? quint64{1} << 63 : static_cast<quint64>(std::numeric_limits<qint64>::max());
  while (i < text.size()) {
    const QChar c = text.at(i);
    if (c < QLatin1Char('0') || c > QLatin1Char('9'))
      break; // The ASCII digit run ends (JS parseInt stops at the first non-digit).
    const quint64 digit = static_cast<quint64>(c.toLatin1() - '0');
    if (value > (kMagnitudeLimit - digit) / 10)
      value = kMagnitudeLimit; // Saturate: parseInt yields a huge double, never NaN.
    else
      value = value * 10 + digit;
    ++i;
  }
  if (i == digitsStart)
    return 0; // No leading digits: JS parseInt -> NaN -> 0.
  if (negative) {
    // The exact |qint64::min()| magnitude maps to qint64::min(); everything
    // below the limit negates losslessly (value <= max is guaranteed here).
    return value == (quint64{1} << 63) ? std::numeric_limits<qint64>::min()
                                       : -static_cast<qint64>(value);
  }
  return static_cast<qint64>(value);
}

void parseTags(const QString& lrc, LrcTag& tag)
{
  // JS: lyric.match(new RegExp(`\\[${tag}:([^\\]]*)]`, 'i')).
  for (const TagField& entry : kTagFields) {
    const QRegularExpression pattern(
      QStringLiteral("\\[%1:([^\\]]*)]").arg(QLatin1String(entry.code)),
      QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(lrc);
    if (match.hasMatch())
      tag.*(entry.field) = match.captured(1);
  }

  // JS: parseInt(this.tags.offset), Number.isNaN -> 0. The C++ port uses
  // leading-decimal parseInt semantics (parseLeadingDecimalInt): trailing
  // junk like "500ms" or "1e3" is accepted, "0x10" stays 0 (see helper).
  const QRegularExpression offsetPattern(QStringLiteral("\\[offset:([^\\]]*)]"),
                                         QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch offsetMatch = offsetPattern.match(lrc);
  if (offsetMatch.hasMatch())
    tag.offsetMs = parseLeadingDecimalInt(offsetMatch.captured(1));
}

void attachExtendedLyric(QHash<QString, LrcLine>& lineMap, const QString& extendedLyric)
{
  // JS parseExtendedLyric: each extended lyric string is its own multi-line
  // block; lines whose text is empty or "//" are ignored (issue #1499).
  const QStringList lines =
    extendedLyric.split(QRegularExpression(QStringLiteral("\\r\\n|\\r|\\n")));
  for (const QString& rawLine : lines) {
    const QString line = rawLine.trimmed();
    if (line.isEmpty())
      continue;
    const QRegularExpressionMatch timeFieldMatch = kTimeFieldExp.match(line);
    if (!timeFieldMatch.hasMatch())
      continue;
    const QString timeField = timeFieldMatch.captured();
    const QString text = line.mid(timeField.length()).trimmed();
    if (text.isEmpty() || text == QStringLiteral("//"))
      continue;
    auto timeMatches = kTimeExp.globalMatch(timeField);
    while (timeMatches.hasNext()) {
      const QString timeStr = formatTimeLabel(timeMatches.next().captured());
      auto it = lineMap.find(timeStr);
      if (it != lineMap.end()) {
        // Same exact-text dedupe rule as parseLines, now uniform across
        // the lrc-embedded and tlrc/rlrc extended-lyrics paths: an
        // exact-text duplicate at one timestamp is never meaningful
        // content, whichever source it came from. No `continue` here —
        // remaining time tags of this same extended line must still be
        // visited, so the duplicate is filtered by the guard instead.
        if (text != it->text && !it->extendedLyrics.contains(text))
          it->extendedLyrics.append(text);
      }
    }
  }
}

QVector<LrcLine> parseLines(const QString& lrc, const QStringList& extendedLyrics)
{
  // JS: this.lyric.split(/\r\n|\r|\n/), each line trimmed.
  const QStringList rawLines = lrc.split(QRegularExpression(QStringLiteral("\\r\\n|\\r|\\n")));

  // Keyed by the stripped time label; insertion order is kept so that equal
  // timestamps keep their JS insertion order after the stable sort.
  QHash<QString, LrcLine> lineMap;
  QVector<QString> insertionOrder;
  // Text with no valid timestamp (broken field, time-less field, or no
  // bracket field at all — bare unsynced text), in file order. Kept OUT of
  // lineMap: statics have no time label, so translations cannot attach to
  // them. isStatic implies timeMs == -1, so the final sort places them
  // before every timed line.
  QVector<LrcLine> staticLines;

  for (const QString& rawLine : rawLines) {
    const QString line = rawLine.trimmed();
    if (line.isEmpty())
      continue; // The JS /g exec on an empty (e.g. trailing) line has no time field.

    // A line may lead with BROKEN time fields (e.g. "[00:00.-1]") that
    // kTimeFieldExp cannot consume; each is peeled and the remainder
    // re-parsed, so "[00:00.-1][00:05.00]text" still yields the timed 5s
    // line with the broken field dropped.
    QString remainder = line;
    for (;;) {
      const QRegularExpressionMatch timeFieldMatch = kTimeFieldExp.match(remainder);
      if (timeFieldMatch.hasMatch()) {
        const QString timeField = timeFieldMatch.captured();
        QString text = remainder.mid(timeField.length()).trimmed();
        // Tail asymmetry: a BROKEN field after the valid run (e.g.
        // "[00:01.00][00:00.-1]text") would otherwise stay as literal
        // text; strip leading broken fields so the line keeps its timed
        // anchor. The strip only ever fires on '-'-containing fields —
        // a '-' -free bracket field was consumed by the greedy valid
        // run already. Caveat: the predicate is a dash-containing
        // bracket field, not strictly a broken timestamp — a
        // pathological literal like "[00:01.00][12-34]x" also loses
        // "[12-34]" (the JS reference would keep it as literal text);
        // accepted trade-off, practically nonexistent in real files.
        while (text.startsWith(QLatin1Char('['))) {
          const QRegularExpressionMatch brokenTail = kInvalidTimeFieldExp.match(text);
          if (!brokenTail.hasMatch())
            break;
          text = text.mid(brokenTail.capturedLength()).trimmed();
        }
        if (text.isEmpty())
          break; // Field run with nothing after it: drop the line.

        auto timeMatches = kTimeExp.globalMatch(timeField);
        if (!timeMatches.hasNext()) {
          // JS: times == null. kTimeExp requires the (?:\.\d{1,3})
          // group, so "[00:01]Hello" yields no valid time — the text
          // becomes a static line instead of being dropped.
          staticLines.append(LrcLine{-1, text, {}, true});
          break;
        }
        while (timeMatches.hasNext()) {
          const QString timeStr = formatTimeLabel(timeMatches.next().captured());
          if (lineMap.contains(timeStr)) {
            LrcLine& lineEntry = lineMap[timeStr];
            // An exact-text duplicate at one timestamp is never
            // meaningful content in any single source either
            // ("[00:01.00]Ah\n[00:01.00]Ah" renders once here where
            // line-player.js would render it twice); the plugin's
            // embedded-tag + sidecar unions just make such
            // VERBATIM duplicates far more common. DELIBERATE,
            // SMALL DEVIATION from line-player.js (which appends
            // unconditionally): skip the duplicate. Different
            // texts (translations) still merge below.
            if (text == lineEntry.text || lineEntry.extendedLyrics.contains(text))
              continue;
            lineEntry.extendedLyrics.append(text);
            continue;
          }

          // JS: timeArr = timeStr.split(':'); too many fields skip the time.
          QStringList timeArr = timeStr.split(QLatin1Char(':'));
          if (timeArr.size() > 3)
            continue;
          while (timeArr.size() < 3)
            timeArr.prepend(QStringLiteral("0"));
          if (timeArr.at(2).contains(QLatin1Char('.'))) {
            const QStringList parts = timeArr.at(2).split(QLatin1Char('.'));
            timeArr[2] = parts.value(0);
            timeArr.append(parts.value(1));
          }

          const qint64 timeMs =
            timeArr.value(0).toLongLong() * 3600000 + timeArr.value(1).toLongLong() * 60000 +
            timeArr.value(2).toLongLong() * 1000 + timeArr.value(3).toLongLong();
          lineMap.insert(timeStr, LrcLine{timeMs, text, {}});
          insertionOrder.append(timeStr);
        }
        break; // Line fully consumed.
      }

      // kTimeFieldExp failed. A time-shaped field it cannot consume is a
      // BROKEN timestamp, NOT a tag: peel ONE such field and re-try the
      // remainder, so a valid field after the broken one is still parsed.
      const QRegularExpressionMatch invalidMatch = kInvalidTimeFieldExp.match(remainder);
      if (!invalidMatch.hasMatch()) {
        // The first character is the discriminator: only a line whose first
        // character is '[' while its content is neither a valid nor a broken
        // time field is dropped here. EVERYTHING else — bare text, a
        // timestamp-less line (e.g. UNSYNCEDLYRICS tags, text-only .lrc
        // sidecars), text merely CARRYING a bracket (trailing time field like
        // "Hello [00:01.00]", or a leading ']') — becomes a STATIC lyric
        // line, the same deliberate deviation as text after a broken
        // timestamp, so a fully unsynced file renders its text instead of
        // "No lyrics". line-player.js drops such lines via `if (result)`.
        if (remainder.startsWith(QLatin1Char('[')))
          break; // Leading '[' that matches neither time-field pattern:
                 // tags ([ti:...], [ar:...], [offset:...]) and unclosed or
                 // malformed brackets — still dropped. Tags are parsed into
                 // LrcTag by parseTags, garbage is meaningless.
        staticLines.append(LrcLine{-1, remainder, {}, true});
        break;
      }
      // A '-' at the START of the field content (e.g. "[-00:05.00]")
      // drops the whole line, keeping lx-music's pre-roll parity; a dash
      // elsewhere (e.g. "[00:-05.00]x", negative seconds) is a broken
      // field and becomes the static line "x" below. The asymmetry is
      // intentional: lx-music drops both forms, so the static fallback
      // is the deliberate deviation and this dash-position check is what
      // preserves the "[-00:05.00]" drop.
      // NB: "[00:00.-1][-00:05.00]x" peels the broken fraction first,
      // then the leading-'-' field drops the WHOLE line (with "x") —
      // correct per spec; don't "fix" this in a refactor.
      if (invalidMatch.captured().at(1) == QLatin1Char('-'))
        break; // A [-mm:ss.xxx]-style pre-roll field: drop the whole line.
      remainder = remainder.mid(invalidMatch.capturedLength()).trimmed();
      if (remainder.isEmpty())
        break; // A broken field with no text after it: drop the line.
      if (!remainder.startsWith(QLatin1Char('['))) {
        // Plain text after the broken field run: a static lyric line —
        // rendered but never visited (never active, never colored,
        // never a scroll target). DELIBERATE DEVIATION from
        // line-player.js, which drops such lines.
        staticLines.append(LrcLine{-1, remainder, {}, true});
        break;
      }
      // The remainder starts with another field run: loop back and re-try.
    }
  }

  for (const QString& extendedLyric : extendedLyrics)
    attachExtendedLyric(lineMap, extendedLyric);

  // Static lines lead the result (their timeMs == -1 sorts them first; the
  // stable sort keeps both their file order and the timed lines' insertion
  // order).
  QVector<LrcLine> lines = staticLines;
  lines.reserve(staticLines.size() + insertionOrder.size());
  for (const QString& key : insertionOrder)
    lines.append(lineMap.value(key));

  // JS: this.lines.sort((a, b) => a.time - b.time). Stable so equal times
  // keep insertion order, matching modern JS engines.
  std::stable_sort(lines.begin(), lines.end(), [](const LrcLine& a, const LrcLine& b) {
    return a.timeMs < b.timeMs;
  });
  return lines;
}

} // namespace

LrcParser::Result LrcParser::parse(const QString& lrc, const QStringList& extendedLyrics)
{
  Result result;
  parseTags(lrc, result.tag);
  result.lines = parseLines(lrc, extendedLyrics);
  return result;
}
