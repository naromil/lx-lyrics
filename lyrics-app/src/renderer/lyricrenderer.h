/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QColor>
#include <QFont>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QFontMetrics;
class QMouseEvent;
class QPainter;
class QRectF;
class QResizeEvent;
class QTimer;
class QVariantAnimation;
class QWheelEvent;

// One renderable lyric line group: a main line plus its optional extended
// lines (translation / romaji). Mirrors the font-player ".line-content" DOM
// node from references/src/common/utils/lyric-font-player/font-player.js.
struct RenderLine {
    QString text;          // main line text (may contain <start,dur> karaoke tags)
    QStringList extended;  // extended lines (translation/romaji)
};

// Paints the lyric list with QPainter, mirroring the reference LyricHorizontal
// / LyricVertical styling. The whole lyric block is centered; the active line
// renders in the played color while inactive lines stay in the unplay color
// (line-by-line highlight per the original lx-music design).
//
// Scrolling ports the reference useLyric.js behavior: the scroll offset keeps
// the active line centered (or pinned top with setScrollAlign(true)), manual
// wheel/drag adjust the offset with auto-scroll suspended for 3 s after the
// last interaction, and setDelayScroll(true) delays the auto-scroll 600 ms and
// then animates it ~300 ms (OutCubic) instead of jumping instantly.
class LyricRenderer : public QWidget {
    Q_OBJECT
public:
    explicit LyricRenderer(QWidget* parent = nullptr);

    void setLines(const QVector<RenderLine>& lines);
    void setActiveLine(int index);              // -1 = none active

    // --- styling (all setters just store; repaint on change) ---
    void setVertical(bool on);                  // false=horizontal (default)
    void setAlign(Qt::Alignment align);         // AlignLeft|AlignHCenter|AlignRight
    void setFontFamily(const QString& family);  // empty = default family
    void setFontSize(int px);                   // 10..80
    void setLineGap(int px);                    // 0..25
    void setOpacityPercent(int percent);        // 6..100 (window/container opacity)
    void setEllipsis(bool on);
    void setZoomActiveLrc(bool on);             // active line font 1.2x, its extended 0.94x
    void setFontWeightFont(bool on);            // write-only: no longer affects rendering (line-by-line mode); retained for API compatibility
    void setFontWeightLine(bool on);            // bold main line
    void setFontWeightExtended(bool on);        // bold extended lines
    void setUnplayColor(const QColor& c);       // default white
    void setPlayedColor(const QColor& c);       // default green rgba(7,197,86,255)
    void setShadowColor(const QColor& c);       // 4px stroke color for every line
    void setShadowFontModeColor(const QColor& c); // write-only: no longer affects rendering (line-by-line mode); retained for API compatibility

    // --- scrolling & interaction (port of useLyric.js) ---
    void setScrollAlign(bool top);   // false=center (default), true=pin active line to top
    void setDelayScroll(bool on);    // false=instant jump (default), true=600ms delay + 300ms animated
    void setUserScrolling(bool on);  // suspend auto-scroll while true; re-arm the resume timer when false
    void setInteractive(bool on);    // gate wheel + drag (window lock state)
    void resetScroll();              // clear offset and any scroll animation (lyric change)

signals:
    // Emitted when a drag starts/stops (reference isMsDown), so the app can
    // swap cursors or suspend other UI while the user grabs the lyrics.
    void userInteractingChanged(bool interacting);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
private:
    void drawTextWithStroke(QPainter& p, const QString& text, const QRectF& rect, const QFont& font, const QColor& fill, const QColor& stroke, int strokeWidth);

    // Per-group geometry for vertical (column) mode, shared by the layout pass
    // and the drawing pass so column widths stay consistent.
    struct VerticalGroupMetrics {
        QFont mainFont;
        QFont extendedFont;
        QString mainText;          // word tags stripped, elided to fit
        QStringList extendedTexts; // word tags stripped, elided to fit
        int mainColumnWidth = 0;
        int mainColumnHeight = 0;
        int groupWidth = 0;
        int groupHeight = 0;
    };

    void paintHorizontal(QPainter& p);
    void paintVertical(QPainter& p);
    void drawHorizontalGroup(QPainter& p, const RenderLine& line, bool isActive, const QRectF& rect);
    void drawVerticalGroup(QPainter& p, const VerticalGroupMetrics& metrics, bool isActive, const QRectF& rect);
    void drawVerticalText(QPainter& p, const QString& text, const QRectF& columnRect, const QFont& font, const QColor& fill, const QColor& stroke, int strokeWidth, int letterSpacing);
    VerticalGroupMetrics measureVerticalGroup(const RenderLine& line, bool isActive) const;
    QFont makeMainFont(bool isActive) const;
    QFont makeExtendedFont(bool isActive) const;
    int textFlags() const;
    QString elideForWidth(const QString& text, const QFontMetrics& fm, int availableWidth) const;
    QString elideForHeight(const QString& text, const QFontMetrics& fm, int availableHeight, int letterSpacing) const;
    int columnWidth(const QString& text, const QFontMetrics& fm) const;
    int columnHeight(const QString& text, const QFontMetrics& fm, int letterSpacing) const;

    // --- scrolling helpers (shared by paint and the scroll target math) ---
    struct HorizontalLayout {
        QVector<qreal> groupHeights;
        qreal blockHeight = 0;
    };
    HorizontalLayout measureHorizontal() const;
    QVector<VerticalGroupMetrics> measureAllVerticalGroups() const;
    qreal verticalBlockWidth(const QVector<VerticalGroupMetrics>& metrics) const;
    // Scroll-range clamp: [0, max(0, blockExtent - viewportExtent)] along the
    // scroll axis (Y for horizontal mode, X for vertical mode).
    qreal maxScrollOffset() const;
    // Offset that puts the active line at its scrollAlign target: centered in
    // the viewport, or 0 ('top' alignment; left edge for vertical mode).
    qreal autoScrollTarget() const;
    void scrollToActiveInstant();
    void scrollToActiveAnimated(int durationMs);
    void suspendAutoScroll();
    void rearmResumeTimer();
    void startDelayScrollTimer();

    QVector<RenderLine> m_lines;
    int m_activeLine = -1;
    bool m_vertical = false;
    Qt::Alignment m_align = Qt::AlignHCenter;
    QString m_fontFamily;
    int m_fontSize = 20;
    int m_lineGap = 15;
    int m_opacityPercent = 95;
    bool m_ellipsis = false;
    bool m_zoomActiveLrc = false;
    bool m_fontWeightLine = true;
    bool m_fontWeightExtended = true;
    QColor m_unplayColor = QColor(Qt::white);
    QColor m_playedColor = QColor(7, 197, 86);
    QColor m_shadowColor = QColor(0, 0, 0, 46);        // rgba(0, 0, 0, 0.18)

    qreal m_scrollOffset = 0;         // manual/auto scroll along the scroll axis
    bool m_scrollAlignTop = false;    // false=center (default), true='top'
    bool m_delayScroll = false;       // false=instant (default), true=600ms delay + 300ms animated
    bool m_userScrolling = false;     // auto-scroll suspended (interaction + 3s resume window)
    bool m_interactive = false;       // gate for wheel + drag handlers
    bool m_dragging = false;
    QPoint m_dragStartPos;
    qreal m_dragStartOffset = 0;
    QTimer* m_resumeTimer = nullptr;        // 3s single-shot, re-armed on every interaction
    QTimer* m_delayScrollTimer = nullptr;   // 600ms single-shot before the animated scroll
    QVariantAnimation* m_scrollAnimation = nullptr; // 300ms OutCubic between offsets
};
