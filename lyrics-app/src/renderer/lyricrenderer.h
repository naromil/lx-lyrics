/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPoint>
#include <QSize>
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
class QWheelEvent;

// One renderable lyric line group: a main line plus its optional extended
// lines (translation / romaji). Mirrors the font-player ".line-content" DOM
// node from references/src/common/utils/lyric-font-player/font-player.js.
struct RenderLine {
    QString text;          // main line text (may contain <start,dur> karaoke tags)
    QStringList extended;  // extended lines (translation/romaji)
};

// Paints the lyric list with QPainter, mirroring the reference LyricHorizontal
// / LyricVertical styling. The lyric block starts below an 80% leading spacer
// (reference .lyricSpace), so the active line can always center in the
// viewport — including the first and last lines (scroll range
// [0, 0.6 * viewport + block]). The active line renders in the played color
// while inactive lines stay in the unplay color (line-by-line highlight per
// the original lx-music design); both the color and the active-line zoom
// transition over 600 ms with per-line progress, so an outgoing line decays
// while the incoming line grows (reference font-size/color .6s ease).
//
// Scrolling ports the reference useLyric.js behavior: every auto-scroll is
// animated with InOutQuad (reference handleScrollY's easeInOutQuad) — normal
// line advances and seek/jump moves take 300 ms, and setDelayScroll(true)
// first waits 600 ms and then animates over 600 ms. Manual wheel/drag adjust
// the offset with auto-scroll suspended for 3 s after the last interaction;
// the scroll offset is only ever set instantly for the initial/reset state,
// when target == current, or while the user is dragging/wheeling.
class LyricRenderer : public QWidget {
    Q_OBJECT
public:
    explicit LyricRenderer(QWidget* parent = nullptr);

    void setLines(const QVector<RenderLine>& lines);
    void setActiveLine(int index);              // -1 = none active
    // Static no-lyrics placeholder mode: the whole block is centered in the
    // viewport (no 80% leading spacer, no scrolling) instead of sitting below
    // the spacer; line styling still follows the settings (align/font/color).
    void setCenteredBlock(bool on);

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
    void setScrollAlign(bool top);   // false=center (default), true=pin active line to the leading edge
    void setDelayScroll(bool on);    // false=immediate 300ms animation (default), true=600ms delay + 600ms animation
    void setUserScrolling(bool on);  // suspend auto-scroll while true; re-arm the resume timer when false
    void setInteractive(bool on);    // gate wheel + drag (window lock state)
    void setPlaying(bool on);        // gate the 3s resume re-center (reference isPlay)
    void resetScroll();              // clear offset and any scroll animation (lyric change)

    // Current active-line zoom progress (0.0 = base scale, 1.0 = fully zoomed
    // at kActiveMainZoom / kActiveExtendedZoom); exposed for tests like
    // LyricWindow::fadeFactor().
    double zoomProgress() const {
        return (m_activeLine >= 0 && m_activeLine < m_lineZoomProgress.size())
            ? m_lineZoomProgress.at(m_activeLine)
            : 0.0;
    }

    // Test accessors for the transition/scroll engines (same role as
    // zoomProgress): per-line played-color / zoom progress and the current
    // scroll offset along the scroll axis.
    double colorProgressForLine(int line) const {
        return (line >= 0 && line < m_lineColorProgress.size())
            ? m_lineColorProgress.at(line)
            : 0.0;
    }
    double zoomProgressForLine(int line) const {
        return (line >= 0 && line < m_lineZoomProgress.size())
            ? m_lineZoomProgress.at(line)
            : 0.0;
    }
    double scrollOffset() const { return m_scrollOffset; }

signals:
    // Emitted when a drag starts/stops (reference isMsDown), so the app can
    // swap cursors or suspend other UI while the user grabs the lyrics.
    void userInteractingChanged(bool interacting);

protected:
    // The renderer owns the lyric area of the window, so layouts must be able
    // to ask it for a real size: without an explicit hint a QVBoxLayout gives a
    // Preferred-policy widget its (invalid) sizeHint and the widget collapses.
    QSize sizeHint() const override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
private:
    void drawTextWithStroke(QPainter& p, const QString& text, const QRectF& rect, const QFont& font, const QColor& fill, const QColor& stroke, int strokeWidth);

    // Per-group geometry for vertical (column) mode, shared by the layout pass
    // and the drawing pass so column widths stay consistent. Each text is held
    // as columns in reading order: wrapped into multiple columns that stack
    // right-to-left (wrap mode) or as a single elided column (ellipsis mode).
    struct VerticalGroupMetrics {
        QFont mainFont;
        QFont extendedFont;
        QStringList mainColumns;             // word tags stripped; wrapped or single elided column
        QVector<QStringList> extendedColumns; // ditto, one entry per extended line
        int groupWidth = 0;
        int groupHeight = 0;
    };

    void paintHorizontal(QPainter& p);
    void paintVertical(QPainter& p);
    void drawHorizontalGroup(QPainter& p, const RenderLine& line, int lineIndex, const QRectF& rect);
    void drawVerticalGroup(QPainter& p, const VerticalGroupMetrics& metrics, int lineIndex, const QRectF& rect);
    void drawVerticalText(QPainter& p, const QString& text, const QRectF& columnRect, const QFont& font, const QColor& fill, const QColor& stroke, int strokeWidth, int letterSpacing);
    // zoomOverrideLine/zoomOverride measure ONE line at a fixed zoom progress
    // (default -1.0 = live progress), used by the isComputeHeight scroll-target
    // compensation to compare the outgoing line's zoomed and base extents.
    VerticalGroupMetrics measureVerticalGroup(const RenderLine& line, int lineIndex,
                                              int zoomOverrideLine = -1, double zoomOverride = 0.0) const;
    QFont makeMainFont(int lineIndex, double zoomOverride = -1.0) const;
    QFont makeExtendedFont(int lineIndex, double zoomOverride = -1.0) const;
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
    HorizontalLayout measureHorizontal(int zoomOverrideLine = -1, double zoomOverride = 0.0) const;
    QVector<VerticalGroupMetrics> measureAllVerticalGroups(int zoomOverrideLine = -1,
                                                           double zoomOverride = 0.0) const;
    qreal verticalBlockWidth(const QVector<VerticalGroupMetrics>& metrics) const;
    // Active-line color transition: one 600 ms OutCubic animation cross-fades
    // the outgoing line from played back to unplay (1.0 -> 0.0) while the
    // incoming line fades in (0.0 -> 1.0), over a shared 0..1 tick
    // (reference .line-mode .font-lrc { transition: color @transition-slow }).
    void animateLineColors(int newActiveLine);
    // Interpolated fill color for one line: played at progress 1.0, unplay at
    // 0.0 (reference --color-lyric-played / --color-lyric-unplay).
    QColor colorForLine(int lineIndex) const;
    // Active-line zoom transition: eases the per-line zoom progress — the
    // incoming line to 1.0 and any outgoing line back to 0.0 over one shared
    // 600 ms tick (reference .lrcActiveZoom font-size .6s ease on
    // .line-mode .font-lrc).
    void startZoomTransition(int newActiveLine);
    // Scroll-range clamp: [0, max(0, kLyricSpaceInnerFactor * viewport +
    // block)] along the scroll axis (Y for horizontal mode, X for vertical
    // mode); the 0.6 * viewport headroom from the 80% spacers keeps the
    // first/last line centered.
    qreal maxScrollOffset() const;
    // Offset that puts the active line at its scrollAlign target: centered in
    // the viewport, or 'top' which pins it to the leading edge — the viewport
    // top for horizontal mode, 2 px from the right edge for vertical mode
    // (reference getOffsetTop: 0 vs contentWidth - lineWidth - 2).
    qreal autoScrollTarget() const;
    void scrollToActiveAnimated(int durationMs);
    void suspendAutoScroll();
    void rearmResumeTimer();
    void startDelayScrollTimer();

    QVector<RenderLine> m_lines;
    int m_activeLine = -1;              // current active line; setLines resets to -1 so the first active-line event re-positions the view
    bool m_vertical = false;
    Qt::Alignment m_align = Qt::AlignHCenter;
    QString m_fontFamily;
    int m_fontSize = 14;
    int m_lineGap = 15;
    int m_opacityPercent = 100;
    bool m_ellipsis = false;
    bool m_zoomActiveLrc = true;
    bool m_fontWeightLine = true;
    bool m_fontWeightExtended = true;
    QColor m_unplayColor = QColor(Qt::white);
    QColor m_playedColor = QColor(7, 197, 86);
    QColor m_shadowColor = QColor(0, 0, 0, 46);        // rgba(0, 0, 0, 0.18)

    qreal m_scrollOffset = 0;         // manual/auto scroll along the scroll axis
    bool m_centeredBlock = false;     // static placeholder: block centered, no scrolling
    bool m_scrollAlignTop = false;    // false=center (default), true='top'
    bool m_delayScroll = false;       // false=immediate 300ms animation (default), true=600ms delay + 600ms animation
    bool m_userScrolling = false;     // auto-scroll suspended (interaction + 3s resume window)
    // The 3 s resume re-centers only while playing (reference isPlay gate in
    // startLyricScrollTimeout). Accepted Finding-4 behavior: the player's
    // internal pause() at the max line (lyricplayer.cpp _handleMaxLine) is not
    // mirrored here — the gate stays open only until the host's
    // set_status(isPlay=false) arrives, matching the reference's isPlay window.
    bool m_playing = false;
    bool m_interactive = false;       // gate for wheel + drag handlers
    bool m_dragging = false;
    QPoint m_dragStartPos;
    qreal m_dragStartOffset = 0;
    QTimer* m_resumeTimer = nullptr;        // 3s single-shot, re-armed on every interaction
    QTimer* m_delayScrollTimer = nullptr;   // 600ms single-shot before the delayed scroll
    int m_prevActiveLine = -1;              // line that just lost active status (zoom compensation)

    // --- scroll engine (port of the reference handleScrollY) ---
    // One 10 ms timer steps the offset with easeInOutQuad. A new target that
    // arrives mid-flight is QUEUED (latest wins): the running animation keeps
    // easing and, once past 75% of its duration, restarts from the current
    // offset toward the queued target — the view never stalls while line
    // changes come faster than the animation (reference lx_scrollNextParams).
    QTimer* m_scrollTimer = nullptr;        // 10ms InOutQuad stepping
    qreal m_scrollStartOffset = 0;          // offset at the start of the current run
    qreal m_scrollTarget = 0;               // target of the current run
    int m_scrollDurationMs = 0;             // duration of the current run
    int m_scrollElapsedMs = 0;              // elapsed time of the current run
    bool m_scrollHasQueuedTarget = false;   // a newer target is waiting
    qreal m_scrollQueuedTarget = 0;
    int m_scrollQueuedDurationMs = 0;

    // --- per-line color/zoom transitions (CSS-transition semantics) ---
    // Each line carries its own (from, to, startMs) transition and continues
    // from its CURRENT interpolated value when retargeted — a line whose
    // active status flips mid-transition never snaps back to the full played
    // color / full zoom, unlike the shared cross-fade it replaces. One 16 ms
    // timer runs only while any transition is active (reference .line-mode
    // .font-lrc { transition: color @transition-slow }).
    struct LineTransition {
        double from = 0.0;    // progress at the start of the current transition
        double to = 1.0;      // target progress
        qint64 startMs = 0;   // transition start on m_transitionClock
        bool active = false;
    };
    QTimer* m_transitionTimer = nullptr;    // 16ms tick while any transition runs
    QElapsedTimer m_transitionClock;        // transition start times (monotonic)
    QVector<LineTransition> m_lineColorTransition; // parallel to m_lineColorProgress
    QVector<LineTransition> m_lineZoomTransition;  // parallel to m_lineZoomProgress

    // Per-line played/unplay color progress (parallel to m_lines; 1.0 =
    // played); the paint path interpolates between unplay and played with it.
    QVector<double> m_lineColorProgress;
    // Per-line active-line zoom progress (parallel to m_lines; 1.0 = fully
    // zoomed); the active main line renders at 1.0 + 0.2*progress and its
    // extended lines at kExtendedScale + 0.14*progress (reference 1.2em /
    // 0.94em targets).
    QVector<double> m_lineZoomProgress;

    void startLineTransition(QVector<LineTransition>& transitions, QVector<double>& progress,
                             int line, double to);
    void stepTransitions();
    void stopScrollAnimation();
    void rebaseScroll(qreal target, int durationMs);
    void scrollTick();
};
