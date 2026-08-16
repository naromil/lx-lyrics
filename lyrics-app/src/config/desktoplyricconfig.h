/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QColor>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>

// Settings store for the desktop lyric feature.
//
// Mirrors lx-music's flat setting['desktopLyric.x'] key access. Defaults are
// the user's tuned preferences, not the upstream file's: they deviate from
// references/src/common/defaultSetting.ts on 8 keys (enable, isAlwaysOnTop,
// isAlwaysOnTopLoop, fullscreenHide, width, style.fontSize, style.opacity,
// style.isZoomActiveLrc); isLock/height and the rest stay verbatim, and x/y
// default to null (auto-position). The store is persisted as JSON at
// QStandardPaths::ConfigLocation + "/lx-lyrics/config.json".
class DesktopLyricConfig : public QObject {
  Q_OBJECT

public:
  explicit DesktopLyricConfig(QObject* parent = nullptr);
  ~DesktopLyricConfig() override;

  // Key-based access (lx-music setting['desktopLyric.x'] pattern).
  // Returns an invalid QVariant for unknown keys.
  QVariant get(const QString& key) const;
  // Returns false for unknown keys (fail fast, no silent typos). Coerces the
  // value to the key's default type, then emits settingChanged/anySettingChanged
  // and schedules a save. A no-op set (same value) stays quiet.
  bool set(const QString& key, const QVariant& value);

  void loadDefaults();
  // Restores one key to its default (e.g. reset color/window position).
  // Returns false for unknown keys.
  bool reset(const QString& key);

  // JSON persistence. load() applies known keys on startup; unknown JSON keys
  // are ignored (forward-compatible) and a missing file is seeded with
  // defaults. load() does not emit settingChanged: subscribers attach after
  // startup and read the final values via get().
  void load();
  // Debounced: schedules the actual write; a 500 ms timer flushes.
  void save();
  // Immediate write (also run at exit so a pending debounce is not lost).
  void flush();

  // Typed convenience getters (thin wrappers around get()).
  bool isEnable() const;
  bool isLock() const;
  bool isAlwaysOnTop() const;
  bool isAlwaysOnTopLoop() const;
  bool isFullscreenHide() const;
  bool isShowTaskbar() const;
  bool isLockScreen() const;
  QString direction() const;
  int fontSize() const;
  int opacity() const;
  QColor unplayColor() const;
  QColor playedColor() const;
  QColor shadowColor() const;
  void setUnplayColor(const QColor& color);
  void setPlayedColor(const QColor& color);
  void setShadowColor(const QColor& color);

  static QString configFilePath();

signals:
  void settingChanged(const QString& key, const QVariant& value);
  void anySettingChanged();

private:
  static QVariant coerceToDefault(const QVariant& defaultValue, const QVariant& value);
  static QColor parseRgbaString(const QString& rgba);
  static QString rgbaStringFromColor(const QColor& color);

  QMap<QString, QVariant> m_defaults;
  QMap<QString, QVariant> m_values;
  QTimer m_saveTimer;
};
