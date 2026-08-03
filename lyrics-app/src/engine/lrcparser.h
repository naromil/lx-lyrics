/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Parsed LRC metadata tags ([ti:...], [ar:...], [al:...], [by:...], [offset:...]).
struct LrcTag {
    QString title;
    QString artist;
    QString album;
    QString by;
    qint64 offsetMs = 0;
};

// One timed lyric line. When two timestamps strip to the same label the later
// text is appended to extendedLyrics instead of creating a new line.
struct LrcLine {
    qint64 timeMs = 0;
    QString text;
    QStringList extendedLyrics;
};

// Pure, static parser for LRC lyrics.
//
// Faithful port of LinePlayer._initTag / _initLines / parseExtendedLyric from
// references/src/common/utils/lyric-font-player/line-player.js. No state, no
// timers: given the same lyric it always returns the same typed Result.
class LrcParser {
public:
    struct Result {
        LrcTag tag;
        QVector<LrcLine> lines;
    };

    static Result parse(const QString& lrc, const QStringList& extendedLyrics = {});
};
