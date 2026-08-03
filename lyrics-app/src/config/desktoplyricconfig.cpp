/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "config/desktoplyricconfig.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

constexpr int kSaveDebounceMs = 500;

bool isMacOS()
{
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}

bool isWindows()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QJsonValue toJsonValue(const QVariant& value)
{
    if (!value.isValid())
        return QJsonValue(QJsonValue::Null); // x/y: null means "let the system position".
    return QJsonValue::fromVariant(value);
}

} // namespace

DesktopLyricConfig::DesktopLyricConfig(QObject* parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &DesktopLyricConfig::flush);

    loadDefaults();
}

DesktopLyricConfig::~DesktopLyricConfig()
{
    // A pending debounced write must not be lost on quit.
    flush();
}

void DesktopLyricConfig::loadDefaults()
{
    // Defaults are the user's tuned preferences, not the upstream file's.
    // They deviate from references/src/common/defaultSetting.ts on: enable,
    // isAlwaysOnTop, isAlwaysOnTopLoop, fullscreenHide, width, style.fontSize,
    // style.opacity, style.isZoomActiveLrc. The remaining keys mirror the
    // file, and platform-dependent keys keep its isWin / !isMac expressions.
    // x/y intentionally default to null (auto-position).
    // Reserved keys (stored, no consumer): enable (window shows unconditionally),
    // isAlwaysOnTopLoop (loop keys off isAlwaysOnTop alone), fullscreenHide (no fullscreen handling).
    m_defaults = {
        { QStringLiteral("desktopLyric.enable"), true },
        { QStringLiteral("desktopLyric.isLock"), false },
        { QStringLiteral("desktopLyric.isAlwaysOnTop"), true },
        { QStringLiteral("desktopLyric.isAlwaysOnTopLoop"), true },
        { QStringLiteral("desktopLyric.isShowTaskbar"), false },
        { QStringLiteral("desktopLyric.pauseHide"), true },
        { QStringLiteral("desktopLyric.audioVisualization"), false },
        { QStringLiteral("desktopLyric.fullscreenHide"), false },
        { QStringLiteral("desktopLyric.isDelayScroll"), true },
        { QStringLiteral("desktopLyric.isLockScreen"), isWindows() },
        { QStringLiteral("desktopLyric.isHoverHide"), false },
        { QStringLiteral("desktopLyric.direction"), QStringLiteral("horizontal") },
        { QStringLiteral("desktopLyric.scrollAlign"), QStringLiteral("center") },
        { QStringLiteral("desktopLyric.width"), 300 },
        { QStringLiteral("desktopLyric.height"), 300 },
        { QStringLiteral("desktopLyric.x"), QVariant() },
        { QStringLiteral("desktopLyric.y"), QVariant() },
        { QStringLiteral("desktopLyric.style.align"), QStringLiteral("center") },
        { QStringLiteral("desktopLyric.style.font"), QString() },
        { QStringLiteral("desktopLyric.style.fontSize"), 14 },
        { QStringLiteral("desktopLyric.style.lineGap"), 15 },
        { QStringLiteral("desktopLyric.style.lyricUnplayColor"), QStringLiteral("rgba(255, 255, 255, 1)") },
        { QStringLiteral("desktopLyric.style.lyricPlayedColor"), QStringLiteral("rgba(7, 197, 86, 1)") },
        { QStringLiteral("desktopLyric.style.lyricShadowColor"), QStringLiteral("rgba(0, 0, 0, 0.18)") },
        { QStringLiteral("desktopLyric.style.opacity"), 100 },
        { QStringLiteral("desktopLyric.style.ellipsis"), false },
        { QStringLiteral("desktopLyric.style.isZoomActiveLrc"), true },
        { QStringLiteral("desktopLyric.style.isFontWeightFont"), true },
        { QStringLiteral("desktopLyric.style.isFontWeightLine"), true },
        { QStringLiteral("desktopLyric.style.isFontWeightExtended"), true },

        // Display-affecting keys outside desktopLyric.* (defaultSetting.ts wins).
        { QStringLiteral("common.langId"), QVariant() },
        { QStringLiteral("player.isShowLyricTranslation"), false },
        { QStringLiteral("player.isShowLyricRoma"), false },
        { QStringLiteral("player.isSwapLyricTranslationAndRoma"), false },
        { QStringLiteral("player.isPlayLxlrc"), !isMacOS() },
        { QStringLiteral("player.playbackRate"), 1.0 },
    };

    m_values = m_defaults;
}

QVariant DesktopLyricConfig::get(const QString& key) const
{
    return m_values.value(key);
}

bool DesktopLyricConfig::set(const QString& key, const QVariant& value)
{
    const auto it = m_values.constFind(key);
    if (it == m_values.constEnd() || !m_defaults.contains(key))
        return false; // Unknown key: fail fast, no silent typos.

    const QVariant coerced = coerceToDefault(m_defaults.value(key), value);
    if (*it == coerced)
        return true; // No-op: keep signals and saves quiet.

    m_values[key] = coerced;
    emit settingChanged(key, coerced);
    emit anySettingChanged();
    save();
    return true;
}

bool DesktopLyricConfig::reset(const QString& key)
{
    if (!m_values.contains(key) || !m_defaults.contains(key))
        return false;
    return set(key, m_defaults.value(key));
}

void DesktopLyricConfig::load()
{
    const QString path = configFilePath();
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.exists()) {
        // First run: seed the config file so defaults are persisted immediately.
        flush();
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "DesktopLyricConfig: cannot read" << path << file.errorString();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "DesktopLyricConfig: invalid config, using defaults:" << path << parseError.errorString();
        return;
    }

    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (!m_defaults.contains(it.key()))
            continue; // Unknown JSON keys are ignored: forward-compatible.
        const QVariant stored = it.value().isNull()
            ? QVariant()
            : coerceToDefault(m_defaults.value(it.key()), it.value().toVariant());
        m_values[it.key()] = stored;
    }
}

void DesktopLyricConfig::save()
{
    m_saveTimer.start();
}

void DesktopLyricConfig::flush()
{
    const QString path = configFilePath();
    if (path.isEmpty())
        return;

    QDir().mkpath(QFileInfo(path).absolutePath());

    // QSaveFile writes to a temp file and renames it into place atomically,
    // so a crash mid-write cannot corrupt the config.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "DesktopLyricConfig: cannot write" << path << file.errorString();
        return;
    }

    QJsonObject obj;
    for (auto it = m_values.constBegin(); it != m_values.constEnd(); ++it)
        obj.insert(it.key(), toJsonValue(it.value()));

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    if (!file.commit())
        qWarning() << "DesktopLyricConfig: commit failed" << path << file.errorString();
}

bool DesktopLyricConfig::isLock() const
{
    return get(QStringLiteral("desktopLyric.isLock")).toBool();
}

bool DesktopLyricConfig::isAlwaysOnTop() const
{
    return get(QStringLiteral("desktopLyric.isAlwaysOnTop")).toBool();
}

bool DesktopLyricConfig::isShowTaskbar() const
{
    return get(QStringLiteral("desktopLyric.isShowTaskbar")).toBool();
}

bool DesktopLyricConfig::isLockScreen() const
{
    return get(QStringLiteral("desktopLyric.isLockScreen")).toBool();
}

QString DesktopLyricConfig::direction() const
{
    return get(QStringLiteral("desktopLyric.direction")).toString();
}

int DesktopLyricConfig::fontSize() const
{
    return get(QStringLiteral("desktopLyric.style.fontSize")).toInt();
}

int DesktopLyricConfig::opacity() const
{
    return get(QStringLiteral("desktopLyric.style.opacity")).toInt();
}

QColor DesktopLyricConfig::unplayColor() const
{
    return parseRgbaString(get(QStringLiteral("desktopLyric.style.lyricUnplayColor")).toString());
}

QColor DesktopLyricConfig::playedColor() const
{
    return parseRgbaString(get(QStringLiteral("desktopLyric.style.lyricPlayedColor")).toString());
}

QColor DesktopLyricConfig::shadowColor() const
{
    return parseRgbaString(get(QStringLiteral("desktopLyric.style.lyricShadowColor")).toString());
}

void DesktopLyricConfig::setUnplayColor(const QColor& color)
{
    set(QStringLiteral("desktopLyric.style.lyricUnplayColor"), rgbaStringFromColor(color));
}

void DesktopLyricConfig::setPlayedColor(const QColor& color)
{
    set(QStringLiteral("desktopLyric.style.lyricPlayedColor"), rgbaStringFromColor(color));
}

void DesktopLyricConfig::setShadowColor(const QColor& color)
{
    set(QStringLiteral("desktopLyric.style.lyricShadowColor"), rgbaStringFromColor(color));
}

QString DesktopLyricConfig::configFilePath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (base.isEmpty())
        return {};
    return base + QStringLiteral("/lx-lyrics/config.json");
}

QVariant DesktopLyricConfig::coerceToDefault(const QVariant& defaultValue, const QVariant& value)
{
    // Parse at the boundary: every stored value is typed to its key's default
    // type, so internal consumers always read trusted QVariant types.
    switch (defaultValue.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
        return value.toInt();
    case QMetaType::Double:
        return value.toDouble();
    case QMetaType::QString:
        return value.toString();
    default:
        // Null-default keys have no fixed type (nullable positions x/y accept
        // a number, common.langId accepts a string). Preserve the value's
        // natural type so it round-trips without drift.
        return value;
    }
}

QColor DesktopLyricConfig::parseRgbaString(const QString& rgba)
{
    // lx-music stores colors as "rgba(r, g, b, a)" with alpha in [0, 1].
    // QColor::fromString would treat the alpha as 0-255, so parse it ourselves.
    static const QRegularExpression pattern(
        QStringLiteral(R"(^rgba\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d+(?:\.\d+)?)\s*\)$)"));
    const QRegularExpressionMatch match = pattern.match(rgba.trimmed());
    if (!match.hasMatch())
        return QColor::fromString(rgba.trimmed()); // Named colors etc. as fallback.

    const int red = qBound(0, match.captured(1).toInt(), 255);
    const int green = qBound(0, match.captured(2).toInt(), 255);
    const int blue = qBound(0, match.captured(3).toInt(), 255);
    const double alpha = qBound(0.0, match.captured(4).toDouble(), 1.0);
    return QColor(red, green, blue, qRound(alpha * 255.0));
}

QString DesktopLyricConfig::rgbaStringFromColor(const QColor& color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'g', 2));
}
