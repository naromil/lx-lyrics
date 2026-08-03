/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class DesktopLyricConfig;

// UI string lookup for the standalone lyrics app.
//
// Mirrors lx-music's JSON language packs (references/src/lang/*.json) embedded
// in the Qt resources at :/translations/<langId>.json — deliberately NOT the
// Qt .ts/.qm translation system. The active pack is resolved from the
// `common.langId` config key (null -> auto-detect from the system locale) and
// en-us is always kept as the fallback.
class TranslationManager : public QObject {
    Q_OBJECT

public:
    explicit TranslationManager(DesktopLyricConfig& config, QObject* parent = nullptr);

signals:
    // Emitted whenever the active language pack changes (e.g. common.langId
    // was updated), so widgets holding translated strings can re-lookup.
    void languageChanged();

public:
    // Returns the translated string for the current language. Missing keys
    // fall back to the en-us pack, then to the key itself (reference i18n.ts
    // behavior). Never returns an empty string for a non-empty key.
    QString tr(const QString& key) const;

    // Resolved language id ("zh-cn", "zh-tw" or "en-us"), even when its pack
    // failed to parse and tr() is falling back to en-us.
    QString currentLanguage() const;

private:
    // Resolves the trusted language id from config/system locale, then reloads
    // the active pack. Called on construction and on `common.langId` changes.
    void applyLanguage();
    QString resolveLanguageId() const;
    static QString languageFromSystemLocale();
    // Parses the embedded pack into a trusted QString map. Returns false (and
    // warns) when the resource is missing or the JSON is invalid; the map is
    // left untouched so the caller can keep the previous fallback.
    static bool loadPack(const QString& langId, QHash<QString, QString>* pack);

    DesktopLyricConfig& m_config;
    QString m_language;
    QHash<QString, QString> m_pack;      // active pack (with en-us fallback content when needed)
    QHash<QString, QString> m_enPack;    // en-us pack, always loaded
};
