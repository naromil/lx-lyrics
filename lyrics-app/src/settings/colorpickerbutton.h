/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QColor>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QStringList>

class DesktopLyricConfig;
class TranslationManager;

// Color swatch button bound to one DesktopLyricConfig key.
//
// Shows the current color (read from config on demand) and opens a picker
// dialog on click: a grid of the reference preset swatches, a per-color
// "Reset to default" action, and Qt's QColorDialog as the advanced fallback.
// Picking a color writes straight through config.set(), so the config stays
// the single source of truth and the rest of the app updates via
// settingChanged. The button only caches a display copy, refreshed from config
// whenever the key changes (including its own writes, which are no-ops on
// equal values). Dialog labels are looked up from the TranslationManager when
// the picker opens, so they always follow the current language.
class ColorPickerButton : public QPushButton {
public:
    explicit ColorPickerButton(const QString& configKey,
                               DesktopLyricConfig& config,
                               TranslationManager& i18n,
                               const QStringList& presetRgbaStrings,
                               const QString& defaultRgba,
                               QWidget* parent = nullptr);

    // Re-read the config value and repaint the swatch.
    void refreshFromConfig();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void openPicker();
    void applyColor(const QColor& color);

    QString m_configKey;
    DesktopLyricConfig& m_config;
    TranslationManager& m_i18n;
    QList<QColor> m_presets;
    QColor m_defaultColor;
    QColor m_color;
};
