/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QPointer>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVariant>
#include <QWidget>

class QEnterEvent;
class QGraphicsOpacityEffect;
class QMouseEvent;
class QMoveEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;
class QVariantAnimation;
class ControlBar;
class DesktopLyricConfig;
class SettingsDialog;
class SpectrumWidget;
class TranslationManager;

// Top-level lyric overlay window.
//
// Mirrors the Electron winLyric module: frameless, translucent, tool-level
// (hidden from the taskbar unless isShowTaskbar), and optionally always on
// top. Geometry and behavior are read from DesktopLyricConfig and kept in
// sync live via settingChanged. The TranslationManager is held only to hand to
// the control bar and settings dialog, which own their translated strings.
class LyricWindow : public QWidget {
    Q_OBJECT

public:
    explicit LyricWindow(DesktopLyricConfig& config, TranslationManager& i18n);

    // Transparent full-bleed child widget; the lyric renderer and control bar
    // attach here in later tasks.
    QWidget* contentContainer() const { return m_contentContainer; }

    // Spectrum visualizer below the control bar (hidden unless
    // desktopLyric.audioVisualization is on). Wired to the host via the
    // SpectrumBridge in app/.
    SpectrumWidget* spectrumWidget() const { return m_spectrumWidget; }

    // Current content fade factor (1.0 = full, 0.05 = pause-faint). Drives
    // both the pane fill and the container opacity effect; exposed for tests.
    double fadeFactor() const { return m_fadeFactor; }

public slots:
    // Lazily creates (once) and shows the modeless settings dialog, raising it
    // if it is already open.
    void openSettingsDialog();

    // Pause-faint (Task D): dim the overlay while paused. Content-level fade
    // only — these never hide or show the window, and never touch window
    // opacity or mouse transparency.
    void faint();
    void unfaint();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // Invisible 8 px edge/corner strip that turns into a resize grip while the
    // lyric window is unlocked. Defined in lyricwindow.cpp; geometry changes
    // are applied here so resize shares the drag path's clamp/save behavior.
    class ResizeHandle;

    enum class ResizeEdge { TopLeft, TopRight, BottomLeft, BottomRight, Left, Right, Top, Bottom };

    void applySetting(const QString& key, const QVariant& value);
    void applyWindowGeometry();
    void applySavedPosition();
    void resizeToConfigSize();
    void clampToAvailableGeometry();
    void applyTransparentForMouseEvents();
    void updateAlwaysOnTopLoop();
    void setWindowFlagKeepingVisible(Qt::WindowType flag, bool on);
    void saveBounds();
    QRect primaryScreenAvailableGeometry() const;
    bool intersectsAnyScreen(const QPoint& topLeft, const QSize& size) const;
    QPoint clampedTopLeft(const QPoint& topLeft, const QSize& windowSize, const QRect& available) const;

    // Content-level pause fade (Task D): animates m_fadeFactor and pushes it
    // into the pane paint and the container opacity effect.
    void animateFadeTo(double target);
    void applyFade();

    // Hover-hide (Task H.1): while desktopLyric.isHoverHide is enabled and the
    // window is locked, a 500 ms cursor poll (reference mouseCheckTools) drives
    // the same faint()/unfaint() fade used by pause-hide — the content dims to
    // kFaintFactor while the cursor is over the locked window. A locked window
    // is mouse-transparent (no enter/leave events fire), so the cursor position
    // is sampled against frameGeometry() instead of relying on hover events.
    void updateHoverHidePolling();
    void pollHoverHide();
    bool isCursorInsideWindow() const;

    void updateResizeHandles();
    void relayoutResizeHandles();
    void beginResize(ResizeEdge edge, const QPoint& globalPos);
    void updateResize(const QPoint& globalPos);
    void endResize();
    QRect resizeGeometryFor(const QPoint& globalPos) const;

    // Compositor-native window drag (Task E): hands the drag to the system so
    // move() never fights the compositor on Wayland/X11. Falls back to the
    // manual Task C drag when the platform cannot move natively.
    void beginWindowDrag(const QPoint& globalPos);
    Qt::Edges nativeEdgesFor(ResizeEdge edge) const;

    DesktopLyricConfig& m_config;
    TranslationManager& m_i18n;
    QWidget* m_contentContainer = nullptr;
    ControlBar* m_controlBar = nullptr;
    SpectrumWidget* m_spectrumWidget = nullptr;
    QTimer* m_alwaysOnTopTimer = nullptr;
    QPointer<SettingsDialog> m_settingsDialog;
    ResizeHandle* m_resizeHandles[8] = {};
    ResizeEdge m_resizeEdge = ResizeEdge::TopLeft;
    bool m_resizing = false;
    QPoint m_resizeStartMouse;
    QRect m_resizeStartGeometry;
    QPoint m_dragOffset;
    bool m_dragging = false;
    bool m_geometryApplied = false;

    // Task E native move/resize: while the compositor drives geometry, the
    // window receives move/resize events; each re-arms this single-shot timer
    // so the final bounds are persisted once the event stream settles (a
    // release event is not reliably delivered during a system drag/resize).
    QTimer* m_nativeOpSaveTimer = nullptr;
    bool m_nativeMoveActive = false;
    bool m_nativeResizeActive = false;

    QVariantAnimation* m_fadeAnim = nullptr;
    QGraphicsOpacityEffect* m_contentEffect = nullptr;
    double m_fadeFactor = 1.0;     // 1.0 = full, kFaintFactor = pause-faint.
    bool m_shouldBeFaint = false;  // Pause-faint state active (last faint()).
    bool m_hoverOverride = false;  // Mouse is over while faint is active.

    // Task H.1 hover-hide: 500 ms poll re-armed on isLock/isHoverHide changes
    // and on show; self-stops while hidden (pollHoverHide guards isVisible).
    QTimer* m_hoverPollTimer = nullptr;
    bool m_hoverHideActive = false; // desktopLyric.isHoverHide && isLock.
};
