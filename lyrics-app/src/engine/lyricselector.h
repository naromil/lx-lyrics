/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once
#include <QString>
#include <QStringList>

struct AwlrcParts {
    QString lyric;    // from lrc: part
    QString tlyric;   // from tlrc: part
    QString rlyric;   // from rlrc: part
    QString lxlyric;  // from awlrc: part
    bool found = false; // true if the container tag was present and at least one part parsed
};

class LyricSelector {
public:
    // Extract [awlrc:...] container from raw lrc text (faithful port of
    // references/src/renderer/worker/main/music.ts parseLyric):
    // - tag regex: /(?:^|\n\s*)\[awlrc:([^\]]+)]/i  -> FIRST match only (JS has no /g),
    //   the matched tag is removed from the lrc text (single removal, then trim).
    // - content split by ','; each part must match /^(lrc|awlrc|tlrc|rlrc):([^,]+)$/i
    //   (case-insensitive key).
    // - value = base64-decode(group2) -> UTF-8 -> trim.
    // - verify before accepting: awlrc part must match /(?:^|\s*)\[\d+:\d+(?:\.\d+)]<\d+,\d+>.+$/m ;
    //   lrc/tlrc/rlrc parts must match /(?:^|\s*)\[\d+:\d+(?:\.\d+)].+$/m
    //   (QRegularExpression with MultilineOption; use PatternOption for the /i flags).
    // - fill AwlrcParts with accepted parts only; found=true if the tag existed.
    static AwlrcParts extractAwlrc(const QString& lrc, QString* strippedLrc = nullptr);

    // Selection state (defaults match lx-music defaultSetting: isPlayLxlrc=true on Linux).
    void setPlayLxlrc(bool on);
    void setIsShowLyricTranslation(bool on);
    void setIsShowLyricRoma(bool on);
    void setIsSwapLyricTranslationAndRoma(bool on);
    // Ingest raw fields (empty strings allowed). Apply extractAwlrc on the lrc field:
    // accepted parts OVERRIDE the corresponding fields (awlrc part -> lxlyric, etc.) and
    // the stripped lrc replaces the lyric field, exactly like parseLyric's spread.
    void setLyrics(const QString& lrc, const QString& tlyric, const QString& rlyric, const QString& lxlyric);

    // Outputs (port of references/src/renderer-lyric/core/lyric.ts setLyric):
    // base = isPlayLxlrc && !lxlyric.isEmpty() ? lxlyric : lyric
    // extended = [rlyric if isShowLyricRoma, tlyric if isShowLyricTranslation]
    //            then reversed if isSwapLyricTranslationAndRoma  (empty strings skipped)
    QString selectedLyric() const;
    QStringList extendedLyrics() const;
    bool hasLyrics() const; // !selectedLyric().isEmpty()
private:
    bool m_playLxlrc = true;
    bool m_showTranslation = false;
    bool m_showRoma = false;
    bool m_swap = false;
    QString m_lyric, m_tlyric, m_rlyric, m_lxlyric;
};
