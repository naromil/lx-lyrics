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

namespace {

// JS: timeFieldExp = /^(?:\[[\d:.]+\])+/g — anchored, so it only consumes the
// leading block of time tags. DELIBERATE DEVIATION, a documented bug fix over
// the JS reference: in line-player.js the /g flag makes the regex stateful via
// exec()'s lastIndex, so consecutive timed lines alternate match/drop (every
// other line fails to match because ^ cannot match once lastIndex advanced).
// The C++ port parses EVERY line; the decision is recorded here.
const QRegularExpression kTimeFieldExp(QStringLiteral("^(?:\\[[\\d:.]+\\])+"));
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
constexpr TagField kTagFields[] = {
    { "ti", &LrcTag::title },
    { "ar", &LrcTag::artist },
    { "al", &LrcTag::album },
    { "by", &LrcTag::by },
};

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

    // JS: parseInt(this.tags.offset), Number.isNaN -> 0.
    const QRegularExpression offsetPattern(QStringLiteral("\\[offset:([^\\]]*)]"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch offsetMatch = offsetPattern.match(lrc);
    if (offsetMatch.hasMatch()) {
        bool ok = false;
        const int parsed = offsetMatch.captured(1).toInt(&ok);
        tag.offsetMs = ok ? parsed : 0;
    }
}

void attachExtendedLyric(QHash<QString, LrcLine>& lineMap, const QString& extendedLyric)
{
    // JS parseExtendedLyric: each extended lyric string is its own multi-line
    // block; lines whose text is empty or "//" are ignored (issue #1499).
    const QStringList lines = extendedLyric.split(QRegularExpression(QStringLiteral("\\r\\n|\\r|\\n")));
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
            if (it != lineMap.end())
                it->extendedLyrics.append(text);
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

    for (const QString& rawLine : rawLines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue; // The JS /g exec on an empty (e.g. trailing) line has no time field.
        const QRegularExpressionMatch timeFieldMatch = kTimeFieldExp.match(line);
        if (!timeFieldMatch.hasMatch())
            continue;
        const QString timeField = timeFieldMatch.captured();
        const QString text = line.mid(timeField.length()).trimmed();
        if (text.isEmpty())
            continue;

        auto timeMatches = kTimeExp.globalMatch(timeField);
        if (!timeMatches.hasNext())
            continue; // JS: times == null. kTimeExp requires the (?:\.\d{1,3})
                      // group, so "[00:01]Hello" produces no match and the line
                      // is silently dropped — faithful to JS.
        while (timeMatches.hasNext()) {
            const QString timeStr = formatTimeLabel(timeMatches.next().captured());
            if (lineMap.contains(timeStr)) {
                lineMap[timeStr].extendedLyrics.append(text);
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

            const qint64 timeMs = timeArr.value(0).toLongLong() * 3600000
                + timeArr.value(1).toLongLong() * 60000
                + timeArr.value(2).toLongLong() * 1000
                + timeArr.value(3).toLongLong();
            lineMap.insert(timeStr, LrcLine{ timeMs, text, {} });
            insertionOrder.append(timeStr);
        }
    }

    for (const QString& extendedLyric : extendedLyrics)
        attachExtendedLyric(lineMap, extendedLyric);

    QVector<LrcLine> lines;
    lines.reserve(insertionOrder.size());
    for (const QString& key : insertionOrder)
        lines.append(lineMap.value(key));

    // JS: this.lines.sort((a, b) => a.time - b.time). Stable so equal times
    // keep insertion order, matching modern JS engines.
    std::stable_sort(lines.begin(), lines.end(),
                     [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });
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
