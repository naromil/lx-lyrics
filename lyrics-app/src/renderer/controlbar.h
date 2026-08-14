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

#include <cstdint>

class QGraphicsOpacityEffect;
class QPaintEvent;
class QPainter;
class QRectF;
class QShowEvent;
class QVariant;
class QVariantAnimation;
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
// no main window, so the close button only requests a close — LyricWindow
// animates the content fade out (300 ms) and then quits the application (the
// lx-music plugin would respawn the app, which does not exist here).
//
// Hover reveal (reference #main:hover .control-bar): the bar is drawn at
// opacity 0 and fades in only while the pointer is over the lyric window.
// LyricWindow drives this via setHovered() from its enter/leave events; the
// bar animates its own QGraphicsOpacityEffect factor 0<->m_revealMaxOpacity
// (default 1.0 = undimmed; LyricWindow pushes the reference body dimming
// 0.8 — the window owns that value, since its container effect now applies
// only the fade; 300 ms in, 500 ms out per reference animate.less). The lock
// visibility gate is unaffected — a locked bar is simply hidden, hover or
// not.
class ControlBar : public QWidget {
  Q_OBJECT

public:
  explicit ControlBar(DesktopLyricConfig& config, TranslationManager& i18n,
                      QWidget* parent = nullptr);

  // Hover reveal: animates the bar's opacity factor to its reveal ceiling
  // m_revealMaxOpacity (hovered) or 0 (not). Safe to call while hidden/
  // locked — the visibility gate still governs presence, and the next show
  // re-seeds the factor from the pointer position.
  void setHovered(bool hovered);

  // Sets the hover-reveal ceiling (clamped 0..1). LyricWindow pushes its
  // body-dimming value (kBodyOpacity) here so the bar matches the reference
  // body { opacity: .8 }; default 1.0 keeps a standalone bar undimmed.
  void setRevealMaxOpacity(qreal max);

signals:
  // Emitted when the settings gear is clicked; LyricWindow opens the
  // modeless settings dialog in response.
  void settingsRequested();

  // Emitted when the close button is clicked; LyricWindow animates the
  // content fade to 0.0 and then quits the app (closeAnimationFinished).
  void closeRequested();

protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  class IconButton; // Defined in controlbar.cpp; paints its icon with QPainter.

  // Tooltip for one bar button: both flip-state translation keys are stored
  // so the tooltip stays translated after a language change. Non-flipping
  // buttons use the same key for both states.
  struct TooltipBinding {
    IconButton* button = nullptr;
    QString keyUnchecked;
    QString keyChecked;
  };

  // The glyphs are flat 16 px vector strokes drawn with QPainter (no icon
  // assets exist in the C++ app).
  enum class Icon : std::uint8_t {
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
    Settings,        // gear (opens the settings dialog)
  };

  IconButton* addIconButton(Icon icon, const QString& tooltipKey);
  IconButton* addIconButton(Icon iconOn, Icon iconOff, const QString& tooltipKeyUnchecked,
                            const QString& tooltipKeyChecked);
  void paintIcon(QPainter& painter, Icon icon, const QRectF& rect) const;
  void syncCheckableStates();
  void retranslate();
  void updateVisibility();
  void onSettingChanged(const QString& key, const QVariant& value);
  void changeFontSize(int delta);
  void changeOpacity(int delta);
  bool isPointerInsideWindow() const;

  DesktopLyricConfig& m_config;
  TranslationManager& m_i18n;
  QHBoxLayout* m_layout = nullptr;
  QVector<TooltipBinding> m_tooltipBindings;
  IconButton* m_zoomButton = nullptr;
  IconButton* m_alwaysOnTopButton = nullptr;
  IconButton* m_settingsButton = nullptr;

  // Hover reveal: the animation drives the effect's opacity factor
  // (reference .control-bar { opacity: 0 } / #main:hover { opacity: 1 }).
  QVariantAnimation* m_hoverAnim = nullptr;
  QGraphicsOpacityEffect* m_hoverEffect = nullptr;
  double m_hoverFactor = 0.0; // current bar opacity factor, 0 = hidden
  // Hover-reveal ceiling: 1.0 undimmed by default; LyricWindow pushes its
  // body-dimming value (kBodyOpacity 0.8) so the bar matches the reference
  // body { opacity: .8 } without a renderer->window include.
  qreal m_revealMaxOpacity = 1.0;
};
