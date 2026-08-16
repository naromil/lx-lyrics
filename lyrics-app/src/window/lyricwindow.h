/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <array>
#include <cstdint>

class QCloseEvent;
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

  // Reference body { opacity: .8 } dimming (App.vue). Applied per component:
  // the renderer's per-line blit dims non-active lines while the active line
  // reaches the full configured color; the pane paint, the spectrum effect
  // and the control bar's hover ceiling use it directly. The container
  // effect applies only the fade.
  static constexpr qreal kBodyOpacity = 0.8;

  // Transparent full-bleed child widget; the lyric renderer and control bar
  // attach here in later tasks.
  QWidget* contentContainer() const { return m_contentContainer; }

  // Spectrum visualizer below the control bar (hidden unless
  // desktopLyric.audioVisualization is on). Wired to the host via the
  // SpectrumBridge in app/.
  SpectrumWidget* spectrumWidget() const { return m_spectrumWidget; }

  // Current content fade factor (1.0 = full, 0.05 = pause- or hover-faint).
  // Drives both the pane fill and the container opacity effect; exposed for
  // tests.
  double fadeFactor() const { return m_fadeFactor; }

  // Current pane background alpha (0.2 normal / 0.0 locked). Drives the
  // pane fill's alpha channel (parity item 1); exposed for tests.
  double paneAlpha() const { return m_paneAlpha; }

  // Lazily creates (once) and shows the modeless settings dialog, raising it
  // if it is already open.
  void openSettingsDialog();

  // Pause-faint (Task D): dim the overlay while paused. Content-level fade
  // only — these never hide or show the window, and never touch window
  // opacity or mouse transparency.
  void faint();
  void unfaint();

  // Host main-window fullscreen state (protocol set_fullscreen). While
  // desktopLyric.fullscreenHide is enabled the lyric window hides when the
  // host's main window is fullscreen and shows again when it leaves
  // (reference main_window_fullscreen event); the recompute runs through
  // updateHiddenByHostConditions().
  void setHostFullscreen(bool isFullscreen);

  // Recomputes whether the window may be visible from the host-visible
  // conditions — desktopLyric.enable off, or desktopLyric.fullscreenHide
  // while the host main window is fullscreen (m_hostFullscreen) — and
  // hides/shows it accordingly (the standalone analog of the reference
  // closeWindow()/createWindow(), winLyric/index.ts). Public so main.cpp can
  // apply the same rule at startup instead of show()ing a window that must
  // stay hidden (enable=false, or the host already fullscreen).
  void updateHiddenByHostConditions();

signals:
  // Emitted when the fade-out close animation completes (the content has
  // reached opacity 0.0); main.cpp quits the app in response. WM/session
  // closes and --exit-on-disconnect stay instant and never emit it.
  void closeAnimationFinished();

  // The user/application initiated a window close: the control-bar X button
  // (through animateClose) or a WM close / Alt+F4 (through closeEvent).
  // Emitted at most once per session (m_closeInitiatedReported guard);
  // main.cpp forwards it to the host as §4 close_requested so the host ends
  // its session WITHOUT respawning the app. Not emitted by quit-time
  // boundaries that never pass a user close (--exit-on-disconnect, app
  // shutdown).
  void closeInitiated();

public slots:
  // Fade-out close: animate the content fade to 0.0 over the usual
  // kFadeAnimationMs, then emit closeAnimationFinished() (which quits the
  // app in main.cpp). Idempotent via the m_closing guard; public so the
  // controller tests can drive it directly. Once closing, hover/pause fades
  // are ignored (faint/unfaint early-exit on m_closing).
  void animateClose();

protected:
  // Hover-poll tick: sample the cursor against frameGeometry() and update the
  // hover-faint source. Protected so the controller tests can drive the
  // cursor-outside branch directly (the poll self-stops while hidden).
  void pollHoverHide();
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
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

  enum class ResizeEdge : std::uint8_t {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Left,
    Right,
    Top,
    Bottom
  };

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
  QPoint clampedTopLeft(const QPoint& topLeft, const QSize& windowSize,
                        const QRect& available) const;

  // Content-level pause fade (Task D): animates m_fadeFactor and pushes it
  // into the pane paint and the container opacity effect.
  void animateFadeTo(double target);
  void applyFade();

  // Once-only close-origin report (closeInitiated): the control-bar close and
  // a WM close may both run in one session (animated close followed by the
  // WM teardown); the host must receive exactly one close_requested.
  void reportCloseInitiated();

  // Pane-fill fade (parity item 1): animates m_paneAlpha between 0.2 and 0.0
  // so the pane background goes fully transparent while the window is locked
  // (reference .lock #main { background-color: transparent }).
  void animatePaneAlphaTo(double target);

  // Hover-hide (Task H.1): while desktopLyric.isHoverHide is enabled and the
  // window is locked, a 500 ms cursor poll (reference mouseCheckTools) drives
  // setHoverFainted() — the content dims to kFaintFactor while the cursor is
  // over the locked window. Pause-faint and hover-faint are independent
  // sources OR'd by updateFaintState(), so the poll never clears a live
  // pause-faint. A locked window is mouse-transparent (no enter/leave events
  // fire), so the cursor position is sampled against frameGeometry() instead
  // of relying on hover events.
  void updateHoverHidePolling();
  void setHoverFainted(bool hovered);
  void updateFaintState();
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
  std::array<ResizeHandle*, 8> m_resizeHandles = {};
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
  double m_fadeFactor = 1.0;    // 1.0 = full, kFaintFactor = pause- or hover-faint.
  bool m_pauseFainted = false;  // Pause-faint source (PauseHide -> faint/unfaint).
  bool m_hoverFainted = false;  // Hover-hide source (cursor over locked window).
  bool m_shouldBeFaint = false; // Either source active (pause || hover).
  bool m_hoverOverride = false; // Mouse is over while faint is active.
  bool m_closing = false;       // Close animation started (X button).
  // closeInitiated already reported once (animated close + WM close must not
  // send the host two close_requested frames).
  bool m_closeInitiatedReported = false;

  QVariantAnimation* m_paneAnim = nullptr;
  double m_paneAlpha = 0.2; // Pane fill alpha: 0.2 normal / 0.0 locked.

  // Task H.1 hover-hide: 500 ms poll re-armed on isLock/isHoverHide changes
  // and on show; self-stops while hidden (pollHoverHide guards isVisible).
  QTimer* m_hoverPollTimer = nullptr;
  bool m_hoverHideActive = false; // desktopLyric.isHoverHide && isLock.

  // Host main window is fullscreen (protocol set_fullscreen). Consumed by
  // updateHiddenByHostConditions under desktopLyric.fullscreenHide.
  bool m_hostFullscreen = false;
};
