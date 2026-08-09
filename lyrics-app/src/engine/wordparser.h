/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QString>
#include <QVector>

// One karaoke word with its time span within a line.
struct LyricWord {
  QString text;
  qint64 startMs = 0;
  qint64 durationMs = 0;
};

// Pure, static parser for karaoke ("font") lyrics: a line is a sequence of
// <startMs,durationMs> tagged words.
//
// Faithful port of FontPlayer._parseLyric / _handleLineParse from
// references/src/common/utils/lyric-font-player/font-player.js (lines 126-189).
class WordParser {
public:
  struct Result {
    bool isLineMode = false;
    QVector<LyricWord> words;
  };

  // Line mode is triggered when any split segment lacks a time tag (JS checks
  // each segment with the non-global timeRxp). In line mode the whole
  // lineText is returned as a single untimed word, exactly like JS
  // _handleLineParse pushes { text: this.lyric }.
  static Result parse(const QString& lineText);

  // True when text starts with a <digits,digits> tag. Matches the Lyric
  // facade's line-mode decision for the first line (index.js line 117:
  // /^<\d+,\d+>/ on lyricLines[0].text).
  static bool hasTimeTags(const QString& text);
};
