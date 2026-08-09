/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "i18n/translationmanager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QVariant>

#include "config/desktoplyricconfig.h"

namespace {

constexpr auto kKeyLanguageId = "common.langId";
constexpr auto kLanguageChineseSimplified = "zh-cn";
constexpr auto kLanguageChineseTraditional = "zh-tw";
constexpr auto kLanguageEnglish = "en-us";
constexpr auto kSupportedLanguages = {kLanguageChineseSimplified, kLanguageChineseTraditional,
                                      kLanguageEnglish};

} // namespace

TranslationManager::TranslationManager(DesktopLyricConfig& config, QObject* parent)
  : QObject(parent)
  , m_config(config)
{
  applyLanguage();

  connect(&m_config, &DesktopLyricConfig::settingChanged, this,
          [this](const QString& key, const QVariant&) {
            if (key == QLatin1String(kKeyLanguageId))
              applyLanguage();
          });
}

QString TranslationManager::tr(const QString& key) const
{
  if (key.isEmpty())
    return key;

  const auto activeIt = m_pack.constFind(key);
  if (activeIt != m_pack.constEnd())
    return activeIt.value();

  const auto fallbackIt = m_enPack.constFind(key);
  if (fallbackIt != m_enPack.constEnd())
    return fallbackIt.value();

  return key; // Unknown key: surface it instead of hiding it with a blank.
}

const QString& TranslationManager::currentLanguage() const
{
  return m_language;
}

void TranslationManager::applyLanguage()
{
  const QString resolved = resolveLanguageId();
  m_language = resolved;

  if (!loadPack(kLanguageEnglish, &m_enPack)) {
    qWarning()
      << "TranslationManager: en-us pack unavailable; keys will fall through to the key itself";
    m_enPack.clear();
  }

  if (resolved == QLatin1String(kLanguageEnglish)) {
    m_pack = m_enPack;
    emit languageChanged();
    return;
  }

  if (loadPack(resolved, &m_pack)) {
    emit languageChanged();
    return;
  }

  qWarning() << "TranslationManager: falling back to en-us for" << resolved;
  m_pack = m_enPack;
  emit languageChanged();
}

QString TranslationManager::resolveLanguageId() const
{
  QString configured = m_config.get(QLatin1String(kKeyLanguageId)).toString().trimmed();
  if (configured.isEmpty())
    return languageFromSystemLocale(); // Auto-detect: no explicit language.

  for (const char* supported : kSupportedLanguages) {
    if (configured == QLatin1String(supported))
      return configured;
  }

  qWarning() << "TranslationManager: unsupported language id" << configured
             << "falling back to en-us";
  return kLanguageEnglish;
}

QString TranslationManager::languageFromSystemLocale()
{
  const QLocale locale = QLocale::system();
  if (locale.language() != QLocale::Chinese)
    return kLanguageEnglish;

  switch (locale.territory()) {
  case QLocale::China:
  case QLocale::Singapore:
  case QLocale::Myanmar: // zh-MY
    return kLanguageChineseSimplified;
  case QLocale::Taiwan:
  case QLocale::HongKong:
  case QLocale::Macau: // zh-MO (QLocale::Macao)
    return kLanguageChineseTraditional;
  default:
    return kLanguageEnglish;
  }
}

bool TranslationManager::loadPack(const QString& langId, QHash<QString, QString>* pack)
{
  const QString path = QStringLiteral(":/translations/%1.json").arg(langId);

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "TranslationManager: cannot open" << path << file.errorString();
    return false;
  }

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    qWarning() << "TranslationManager: invalid JSON in" << path << parseError.errorString();
    return false;
  }

  QHash<QString, QString> parsed;
  const QJsonObject obj = doc.object();
  parsed.reserve(obj.size());
  for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
    if (!it.value().isString()) {
      qWarning() << "TranslationManager: non-string value for key" << it.key() << "in" << path;
      continue;
    }
    parsed.insert(it.key(), it.value().toString());
  }

  pack->swap(parsed); // Only commit a fully parsed pack (parse at the boundary).
  return true;
}
