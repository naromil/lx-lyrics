/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QPaintEvent;
class QPainter;
class QRectF;
class QShowEvent;
class QVariant;
class QHBoxLayout;
class DesktopLyricConfig;
class TranslationManager;

// Floating control strip at the top of the lyric window (port of the
// reference ControlBar.vue). Visible only while the lyric window is unlocked;
// once locked, the bar hides and the settings dialog (Ctrl+,) is the only way
// back.
//
// DesktopLyricConfig is the single source of truth. Every button reads the
// current value on press, clamps it, and writes the result through
// config.set() — nothing is cached. The bar also listens to settingChanged to
// re-sync its checkable buttons and its own visibility, so external changes
// (settings dialog) stay in sync. Tooltips are looked up from the
// TranslationManager at show time and re-looked-up on language changes.
//
// Close is the only action that does not touch config: the standalone app has
// no main window, so closing the lyric quits the application (the lx-music
// plugin would respawn the app, which does not exist here).
class ControlBar : public QWidget {
    Q_OBJECT

public:
    explicit ControlBar(DesktopLyricConfig& config, TranslationManager& i18n, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    class IconButton; // Defined in controlbar.cpp; paints its icon with QPainter.

    // Tooltip for one bar button: both flip-state translation keys are stored
    // so the tooltip stays translated after a language change. Non-flipping
    // buttons use the same key for both states.
    struct TooltipBinding {
        IconButton* button;
        QString keyUnchecked;
        QString keyChecked;
    };

    // The glyphs are flat 16 px vector strokes drawn with QPainter (no icon
    // assets exist in the C++ app).
    enum class Icon {
        Close,
        Lock,
        FontIncrease,    // "A+"
        FontDecrease,    // "A-"
        OpacityIncrease, // half-filled circle with a plus
        OpacityDecrease, // half-filled circle with a minus
        Zoom,            // vibration glyph (zoom off)
        ZoomOff,         // vibration glyph with a slash (zoom on)
        PinOn,           // thumbtack (always-on-top on)
        PinOff,          // thumbtack with a slash (always-on-top off)
    };

    IconButton* addIconButton(Icon icon, const QString& tooltipKey);
    IconButton* addIconButton(Icon iconOn, Icon iconOff, const QString& tooltipKeyUnchecked, const QString& tooltipKeyChecked);
    void paintIcon(QPainter& painter, Icon icon, const QRectF& rect) const;
    void syncCheckableStates();
    void retranslate();
    void updateVisibility();
    void onSettingChanged(const QString& key, const QVariant& value);
    void changeFontSize(int delta);
    void changeOpacity(int delta);

    DesktopLyricConfig& m_config;
    TranslationManager& m_i18n;
    QHBoxLayout* m_layout = nullptr;
    QVector<TooltipBinding> m_tooltipBindings;
    IconButton* m_zoomButton = nullptr;
    IconButton* m_alwaysOnTopButton = nullptr;
};
