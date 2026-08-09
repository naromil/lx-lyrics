/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop
 * (https://github.com/lyswhut/lx-music-desktop), Copyright (c) lyswhut,
 * licensed under Apache-2.0. Copyright (c) 2026 LX Lyrics contributors.
 */
#include "renderer/lyricrenderer.h"

#include <QEasingCurve>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QRectF>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <array>
#include <cmath>
#include <cstddef>

namespace {

// Vertical line-mode letter-spacing (CSS .line-content.line-mode, vertical).
constexpr int kVerticalLetterSpacing = 5;
// Scroll timing (reference useLyric.js): 3 s of manual scrolling before the
// auto-scroll resumes; a delayed scroll waits 600 ms then animates over
// 600 ms (handleScrollLrc(600)), every other auto-scroll animates over 300 ms
// (handleScrollLrc()'s default). The animation steps at 10 ms intervals like
// the reference handleScrollY (increment = 10).
constexpr int kResumeDelayMs = 3000;
constexpr int kDelayScrollMs = 600; // pre-delay before a delayed scroll
constexpr int kDelayScrollAnimationMs = 600; // duration of a delayed scroll
constexpr int kScrollAnimationMs = 300;      // duration of a normal auto-scroll
constexpr int kScrollStepMs = 10;            // handleScrollY increment
// A new scroll target arriving mid-flight is queued; the running animation
// only restarts toward it once past 75% of its duration (reference
// animateScroll: if (element.lx_scrollNextParams && currentTime >
// duration*0.75)).
constexpr qreal kScrollRetargetThreshold = 0.75;
// Active-line zoom factors (reference .lrcActiveZoom).
constexpr qreal kActiveMainZoom = 1.2;
constexpr qreal kActiveExtendedZoom = 0.94;
// Color/zoom transition duration (reference .line-mode .font-lrc transition:
// color/font-size .6s ease); per-line transitions tick at ~60 fps.
constexpr int kTransitionAnimationMs = 600;
constexpr int kTransitionTickMs = 16;
// Extended lines render at 0.8x the main size (reference .extended).
constexpr qreal kExtendedScale = 0.8;
// Reference stroke tables (layout.less .stroke3/.stroke4): each comma entry of
// the CSS text-shadow stack is one zero-blur glyph copy drawn at an (x, y)
// offset under the text fill, alpha-composited separately — so duplicate
// offsets are intentional extra alpha passes. Offsets are em-based per CSS
// text-shadow: they scale with the element's own font pixel size (the
// active-line zoom and the 0.8x extended scale included); px offsets are fixed
// device pixels. stroke3 is the horizontal line-mode halo (32 em offsets,
// layout.less lines 120-151), stroke4 the vertical line-mode halo (em + px
// offsets, layout.less lines 52-76).
// Horizontal line-mode strokes use the shadow color at 51% alpha
// (reference useTheme.ts: --color-lyric-shadow-font-mode =
// RGB_Alpha_Shade(0.49, shadowColor), which stroke3 hardcodes for every
// offset); vertical strokes use the full color (stroke4).
constexpr qreal kHorizontalStrokeAlphaFactor = 0.51;
// Always-centered lyric block (reference .lyricSpace height:80%): the block
// starts below an 80% leading spacer and gains an 80% trailing spacer, so the
// scrollable extent is 1.6 * viewport + block and the usable range holds
// kLyricSpaceInnerFactor * viewport of headroom beyond the block — even the
// first/last line can center.
constexpr double kLyricSpaceRatio = 0.8;
constexpr double kLyricSpaceInnerFactor = 0.6;
// The renderer never collapses below one visible line inside its layout.
constexpr int kMinimumHeight = 80;
// Default sizeHint: a real value so layouts that consult the hint first (or
// place the widget outside a stretch) still give it room to paint.
const QSize kSizeHint(400, 120);
// Transparent halo kept around every cached line pixmap: the em-offset stroke
// passes draw up to 0.04em + 1 px (worst case 0.04 * 80 px max font * 1.2x
// zoom + 1 px ≈ 5 px) beyond the group rect, and the old direct paint clipped
// only at the WIDGET edge — the halo must not be cut at group boundaries.
// The blit compensates by drawing the pixmap kCacheHaloPadPx up-left of the
// group rect.
constexpr qreal kCacheHaloPadPx = 8.0;

// Word-level karaoke tags are stripped; display is line-by-line per the
// original lx-music design. Removes every <digits,digits> sequence (JS
// timeRxpAll: /<(\d+),(\d+)>/g); extended lines arrive pre-tagged too. The
// pattern lives in a function-local static: a namespace-scope
// QRegularExpression could throw during static init
// (bugprone-throwing-static-initialization).
QString stripWordTags(QString text) {
    static const QRegularExpression kKaraokeTagRxp(
        QStringLiteral("<\\d+,\\d+>"));
    text.remove(kKaraokeTagRxp);
    return text;
}

// Reference easeInOutQuad (renderer.ts): t = elapsed, b = start, c = change,
// d = duration.
qreal easeInOutQuad(qreal t, qreal b, qreal c, qreal d) {
    t /= d / 2.0;
    if (t < 1)
        return c / 2.0 * t * t + b;
    t -= 1;
    return -c / 2.0 * (t * (t - 2) - 1) + b;
}

// CSS 'ease' approximation shared by the color/zoom transitions
// (reference @transition-slow: .6s ease), identical to the easing the old
// shared QVariantAnimation cross-fade used.
qreal easeOutCubic(qreal t) {
    const qreal u = 1.0 - t;
    return 1.0 - u * u * u;
}

// Reference useTheme.ts RGB_Alpha_Shade(0.49, color): same RGB, alpha scaled
// by (1 - 0.49) = 0.51 (see kHorizontalStrokeAlphaFactor).
QColor shadedShadowColor(const QColor &color) {
    QColor shaded = color;
    shaded.setAlphaF(color.alphaF() * kHorizontalStrokeAlphaFactor);
    return shaded;
}

// One entry of the reference stroke3/stroke4 text-shadow stacks: a glyph-copy
// offset. Em offsets scale with the rendering font's pixel size (CSS em = the
// element's own font-size); px offsets are fixed device pixels — per entry,
// per axis.
struct StrokeOffset {
    qreal x = 0.0;
    bool xIsEm = false;
    qreal y = 0.0;
    bool yIsEm = false;
};

// Reference .stroke3(@color) (layout.less lines 120-151): the horizontal
// line-mode halo, 32 em offsets drawn in the 51%-alpha font-mode shadow shade
// (--color-lyric-shadow-font-mode). Duplicates are kept verbatim because each
// entry is a separate alpha-composited text-shadow pass.
constexpr std::array<StrokeOffset, 32> kStroke3Offsets = {{
    {0.04, true, 0.04, true},   {0.04, true, -0.03, true},
    {-0.04, true, -0.03, true}, {-0.04, true, 0.04, true},
    {0.04, true, 0.01, true},   {0.04, true, -0.01, true},
    {-0.04, true, -0.01, true}, {-0.04, true, 0.01, true},
    {0.04, true, 0.00, true},   {0.04, true, 0.00, true},
    {-0.04, true, 0.00, true},  {-0.04, true, 0.00, true},
    {0.01, true, 0.04, true},   {0.01, true, -0.03, true},
    {-0.01, true, -0.03, true}, {-0.01, true, 0.04, true},
    {0.01, true, 0.01, true},   {0.01, true, -0.01, true},
    {-0.01, true, -0.01, true}, {-0.01, true, 0.01, true},
    {0.01, true, 0.00, true},   {0.01, true, 0.00, true},
    {-0.01, true, 0.00, true},  {-0.01, true, 0.00, true},
    {0.00, true, 0.04, true},   {0.00, true, -0.03, true},
    {0.00, true, -0.03, true},  {0.00, true, 0.04, true},
    {0.00, true, 0.01, true},   {0.00, true, -0.01, true},
    {0.00, true, -0.01, true},  {0.00, true, 0.01, true},
}};

// Reference .stroke4(@color) (layout.less lines 52-76): the vertical line-mode
// halo, 17 em + px offsets drawn in the full shadow color
// (--color-lyric-shadow). Duplicates are kept verbatim because each entry is a
// separate alpha-composited text-shadow pass.
constexpr std::array<StrokeOffset, 17> kStroke4Offsets = {{
    {0.02, true, -0.02, true}, // 0.02em -0.02em
    {-0.02, true, -1, false},  // -0.02em -1px
    {-0.02, true, 1, false},   // -0.02em 1px
    {0.02, true, 1, false},    // 0.02em 1px
    {0.02, true, -1, false},   // 0.02em -1px
    {-0.02, true, 0, false},   // -0.02em 0px
    {-0.02, true, 0, false},   // -0.02em 0px
    {0.02, true, 0, false},    // 0.02em 0px
    {0.02, true, 0, false},    // 0.02em 0px
    {-1, false, -1, false},    // -1px -1px
    {-1, false, 1, false},     // -1px 1px
    {1, false, 1, false},      // 1px 1px
    {1, false, -1, false},     // 1px -1px
    {-1, false, 0, false},     // -1px 0px
    {-1, false, 0, false},     // -1px 0px
    {1, false, 0, false},      // 1px 0px
    {1, false, 0, false},      // 1px 0px
}};

// Horizontal gap formulas from the reference --line-extended-gap / --line-gap.
qreal horizontalExtendedGap(int lineGap) { return lineGap / 3.0; }

qreal verticalGroupGap(int lineGap) { return std::ceil(lineGap * 1.06); }

qreal verticalExtendedGap(int lineGap) {
    return std::ceil(lineGap * 1.06 / 8.0);
}

// Line-by-line highlight color: linear interpolation between the unplay and
// played colors over the line's 0..1 color progress (per-channel qRound lerp,
// mirroring the CSS color transition between --color-lyric-unplay and
// --color-lyric-played).
QColor interpolateColor(const QColor &unplay, const QColor &played, double p) {
    if (p <= 0.0)
        return unplay;
    if (p >= 1.0)
        return played;
    return QColor(
        qRound(unplay.red() + (played.red() - unplay.red()) * p),
        qRound(unplay.green() + (played.green() - unplay.green()) * p),
        qRound(unplay.blue() + (played.blue() - unplay.blue()) * p),
        qRound(unplay.alpha() + (played.alpha() - unplay.alpha()) * p));
}

// Break a single word that does not fit a line width into character chunks,
// each at most `width` wide. Mirrors CSS word-break:break-all (the ellipsis
// clamp variant) / overflow-wrap:break-word (wraps the word at the overflow
// point): CJK characters split individually, long latin words break at any
// character. `scale` multiplies the advances (active-line zoom) so the chunked
// text still fits the unscaled width after it grows.
QStringList breakLongWord(const QString &word, const QFontMetrics &fm,
                          int width, qreal scale = 1.0) {
    QStringList chunks;
    QString chunk;
    qreal chunkWidth = 0;
    for (const QChar ch : word) {
        const qreal chWidth = fm.horizontalAdvance(ch) * scale;
        if (!chunk.isEmpty() && chunkWidth + chWidth > width) {
            chunks << chunk;
            chunk.clear();
            chunkWidth = 0;
        }
        chunk += ch;
        chunkWidth += chWidth;
    }
    if (!chunk.isEmpty())
        chunks << chunk;
    return chunks;
}

// Greedy word wrap on spaces (reference .line-content overflow-wrap:break-word
// — no separate wrap toggle; ellipsis only switches the render to single-line
// ElideRight). A word that alone overflows the line width is broken
// character-by-character; empty / whitespace-only input stays a single entry.
// `scale` multiplies the advances (active-line zoom) so a zoomed row still
// wraps at the unscaled width.
QStringList wrapForWidth(const QString &text, const QFontMetrics &fm, int width,
                         qreal scale = 1.0) {
    if (width <= 0 || text.isEmpty() || text.trimmed().isEmpty())
        return {text};

    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const qreal spaceWidth = fm.horizontalAdvance(QLatin1Char(' ')) * scale;
    QStringList rows;
    QString row;
    qreal rowWidth = 0;

    auto startRow = [&](const QString &first) {
        row = first;
        rowWidth = fm.horizontalAdvance(first) * scale;
    };
    auto commitRow = [&]() {
        if (!row.isEmpty()) {
            rows << row;
            row.clear();
            rowWidth = 0;
        }
    };
    auto takeWord = [&](const QString &word) {
        const QStringList chunks = breakLongWord(word, fm, width, scale);
        startRow(chunks.first());
        for (int i = 1; i < chunks.size(); ++i) {
            commitRow();
            startRow(chunks.at(i));
        }
    };

    for (const QString &word : words) {
        const qreal wordWidth = fm.horizontalAdvance(word) * scale;
        if (row.isEmpty()) {
            if (wordWidth <= width)
                startRow(word);
            else
                takeWord(word);
            continue;
        }
        if (rowWidth + spaceWidth + wordWidth <= width) {
            row += QLatin1Char(' ') + word;
            rowWidth += spaceWidth + wordWidth;
        } else {
            commitRow();
            if (wordWidth <= width)
                startRow(word);
            else
                takeWord(word);
        }
    }
    commitRow();
    return rows;
}

// Greedy word wrap on spaces along the vertical axis (reference .line-content
// overflow-wrap:break-word in writing-mode: vertical-rl): each character owns
// one line box (fm.height() * scale + letterSpacing) and a wrapped "line"
// (column) breaks when the next word would exceed availableHeight. A word that
// alone does not fit a column is chunked character-by-character. Empty /
// whitespace-only input stays a single entry, like wrapForWidth. `scale` is
// the active-line zoom: the line box grows fractionally while the
// letter-spacing stays fixed (CSS letter-spacing does not zoom).
QStringList wrapForHeight(const QString &text, const QFontMetrics &fm,
                          int availableHeight, int letterSpacing,
                          qreal scale = 1.0) {
    if (availableHeight <= 0 || text.isEmpty() || text.trimmed().isEmpty())
        return {text};

    const qreal step = fm.height() * scale + letterSpacing;
    // Max chars per column so that columnHeight(n) = n*step - letterSpacing
    // never exceeds availableHeight.
    const int maxChars = int((availableHeight + letterSpacing) / step);
    if (maxChars <= 0)
        return {text};

    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList columns;
    QString column;
    int columnChars = 0;

    auto startColumn = [&](const QString &first) {
        column = first;
        columnChars = first.size();
    };
    auto commitColumn = [&]() {
        if (!column.isEmpty()) {
            columns << column;
            column.clear();
            columnChars = 0;
        }
    };
    // A word longer than maxChars breaks into maxChars-char chunks.
    auto takeWord = [&](const QString &word) {
        startColumn(word.left(maxChars));
        for (int i = maxChars; i < word.size(); i += maxChars) {
            commitColumn();
            startColumn(word.mid(i, maxChars));
        }
    };

    for (const QString &word : words) {
        if (column.isEmpty()) {
            if (word.size() <= maxChars)
                startColumn(word);
            else
                takeWord(word);
            continue;
        }
        if (columnChars + 1 + word.size() <= maxChars) {
            column += QLatin1Char(' ') + word;
            columnChars += 1 + word.size();
        } else {
            commitColumn();
            if (word.size() <= maxChars)
                startColumn(word);
            else
                takeWord(word);
        }
    }
    commitColumn();
    return columns;
}

} // namespace

LyricRenderer::LyricRenderer(QWidget *parent) : QWidget(parent) {
    // Own the lyric area: expand into whatever space the layout leaves and
    // never collapse below one visible line (default Preferred policy + an
    // invalid default sizeHint lets a QVBoxLayout shrink this widget to ~0).
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(kMinimumHeight);

    // Auto-scroll resumes 3 s after the last wheel/drag interaction (reference
    // startLyricScrollTimeout); single-shot and re-armed on every interaction.
    // The reference gates the resume on isPlay; the controller mirrors that
    // with setPlaying() so a paused/stopped track never re-centers.
    m_resumeTimer = new QTimer(this);
    m_resumeTimer->setSingleShot(true);
    m_resumeTimer->setInterval(kResumeDelayMs);
    connect(m_resumeTimer, &QTimer::timeout, this, [this] {
        m_userScrolling = false;
        if (!m_playing)
            return; // Reference startLyricScrollTimeout: if (!isPlay.value)
                    // return.
        scrollToActiveAnimated(kScrollAnimationMs);
    });

    // isDelayScroll: the active-line change waits 600 ms, then scrolls
    // smoothly (reference scrollLine's setTimeout + handleScrollLrc(600)).
    // The wait is NOT re-armed on consecutive changes: a burst of fast lines
    // keeps one pending roll that retargets to the current line when it fires
    // (the reference's stacked scrollLine timeouts roll the same way), so the
    // view never freezes mid-burst and then suddenly jumps to catch up.
    m_delayScrollTimer = new QTimer(this);
    m_delayScrollTimer->setSingleShot(true);
    m_delayScrollTimer->setInterval(kDelayScrollMs);
    connect(m_delayScrollTimer, &QTimer::timeout, this, [this] {
        if (m_userScrolling)
            return; // User grabbed the lyrics meanwhile: leave it to the resume
                    // timer.
        scrollToActiveAnimated(kDelayScrollAnimationMs);
    });

    // Scroll engine (port of the reference handleScrollY): a 10 ms timer steps
    // the offset with easeInOutQuad. A new target arriving mid-flight is
    // queued; the running animation continues and restarts toward it once past
    // 75% of its duration (see rebaseScroll / scrollTick) — the offset keeps
    // advancing even while line changes outpace the animation.
    m_scrollTimer = new QTimer(this);
    m_scrollTimer->setInterval(kScrollStepMs);
    connect(m_scrollTimer, &QTimer::timeout, this, [this] { scrollTick(); });

    // Per-line color/zoom transitions: a 16 ms tick runs only while any line's
    // transition is active (see startLineTransition / stepTransitions). Each
    // line continues from its CURRENT interpolated value toward its target, so
    // a line that loses active status mid-transition fades from where it is —
    // never snapping back to the full played color / full zoom (reference
    // .line-mode .font-lrc { transition: color/font-size @transition-slow }).
    m_transitionTimer = new QTimer(this);
    m_transitionTimer->setInterval(kTransitionTickMs);
    connect(m_transitionTimer, &QTimer::timeout, this,
            [this] { stepTransitions(); });
    m_transitionClock.start();
}

QSize LyricRenderer::sizeHint() const { return kSizeHint; }

void LyricRenderer::setLines(const QVector<RenderLine> &lines) {
    m_lines = lines;
    // Rebuild the per-line color/zoom progress with the lines: everything
    // starts unplayed and unzoomed (a lyric change does not animate).
    // Transitions are dropped (fresh lyric, fresh state); m_prevActiveLine
    // resets because the zoom compensation must not reference a line of the
    // previous lyric.
    m_lineColorProgress.fill(0.0, m_lines.size());
    m_lineZoomProgress.fill(0.0, m_lines.size());
    m_lineColorTransition.fill(LineTransition{}, m_lines.size());
    m_lineZoomTransition.fill(LineTransition{}, m_lines.size());
    m_prevActiveLine = -1;
    if (m_transitionTimer)
        m_transitionTimer->stop();
    resetScroll();
    // The port resets the scroll and the active line, then the follow-up
    // setActiveLine(currentLine()) positions the view at the new current
    // line's CENTERED target — line 0 is centered, not parked at the top.
    // This reset is a port choice, not reference behavior: on a non-empty
    // lyric change the reference keeps the previous scrollTop and glides to
    // the new line via initLrc -> nextTick -> handleScrollLrc (it only
    // scrolls to the top for the empty/cleared case). Resetting m_activeLine
    // here also guarantees the follow-up call proceeds (the same-line
    // early-return in setActiveLine would otherwise swallow the centering
    // scroll when the old active line was also 0).
    m_activeLine = -1;
    invalidateLineCaches(); // Fresh content: every cached raster is stale.
    update();
}

void LyricRenderer::setCenteredBlock(bool on) {
    if (m_centeredBlock == on)
        return;
    m_centeredBlock = on;
    if (on) {
        // Static placeholder: no leading spacer, no scroll offset.
        resetScroll();
        m_userScrolling = false;
    }
    update();
}

void LyricRenderer::setActiveLine(int index) {
    // Parse at the boundary: out-of-range indexes mean "no active line", and a
    // STATIC line can never become active/colored — structural guarantee at
    // the renderer boundary, regardless of caller. The player already never
    // emits static indices; this clamps any stray push anyway.
    const int clamped =
        (index >= 0 && index < m_lines.size() && !m_lines[index].isStatic)
            ? index
            : -1;
    if (m_activeLine == clamped)
        return;
    const int oldLine = m_activeLine;
    m_prevActiveLine = oldLine; // Zoom compensation reads the outgoing line.

    // Both transitions read m_activeLine as the outgoing line, so they run
    // before the active line is re-pointed. They only repaint (color/zoom) and
    // never touch the scroll state below.
    animateLineColors(clamped);
    startZoomTransition(clamped);
    m_activeLine = clamped;
    update();

    // Both transitions are set up before the scroll math so the auto-scroll
    // target measures the layout at the base scale (the incoming line is at
    // zoom progress 0) — matching the reference, whose isComputeHeight
    // (layout-with-zoom) only holds when isDelayScroll is OFF (the default
    // here is on).
    // Reference scrollLine guard (line 193): a negative/absent line never
    // scrolls. The first line-0 after a setLines is NOT swallowed — like the
    // reference initLrc (prevActiveLine=0, isSetedLines=true, then
    // handleScrollLrc) it scrolls to the new lyric's centered line-0 target.
    if (clamped < 0 || m_lines.isEmpty())
        return; // reference scrollLine: line < 0 || !lines.length never scrolls

    if (m_userScrolling)
        return; // Suspended: the resume timer re-centers to the new line later.

    // Only consecutive +1 steps honour the delay (reference scrollLine); jumps,
    // seeks and the first line animate immediately over the normal 300 ms.
    const bool consecutiveStep = oldLine >= 0 && clamped == oldLine + 1;
    if (m_delayScroll && consecutiveStep)
        startDelayScrollTimer();
    else
        scrollToActiveAnimated(kScrollAnimationMs);
}

void LyricRenderer::setZoomProgressForLine(int line, double progress) {
    if (line < 0 || line >= m_lineZoomProgress.size())
        return;
    // Test accessor: freeze the transition engine and pin one line's zoom
    // progress so a test can render deterministic mid-transition frames (the
    // painted glyphs must grow continuously, not in pixel steps).
    m_transitionTimer->stop();
    m_lineZoomTransition.fill(LineTransition{}, m_lines.size());
    m_lineZoomProgress[line] = qBound(0.0, progress, 1.0);
    if (line < m_lineCache.size())
        m_lineCache[line].dirty =
            true; // The pinned progress may jump mid-flight.
    update();
}

void LyricRenderer::setVertical(bool on) {
    m_vertical = on;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setAlign(Qt::Alignment align) {
    m_align = align;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setFontFamily(const QString &family) {
    m_fontFamily = family;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setFontSize(int px) {
    m_fontSize = qBound(10, px, 80);
    invalidateLineCaches();
    update();
}

void LyricRenderer::setLineGap(int px) {
    m_lineGap = qBound(0, px, 25);
    invalidateLineCaches();
    update();
}

void LyricRenderer::setOpacityPercent(int percent) {
    m_opacityPercent = qBound(6, percent, 100);
    update();
}

void LyricRenderer::setEllipsis(bool on) {
    m_ellipsis = on;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setZoomActiveLrc(bool on) {
    if (m_zoomActiveLrc == on)
        return;
    m_zoomActiveLrc = on;
    invalidateLineCaches();
    if (on) {
        // Toggled on with a line active: grow it in over the transition
        // (reference class add -> CSS font-size transition).
        if (m_activeLine >= 0)
            startZoomTransition(m_activeLine);
    } else {
        // Toggled off: no transition — every line renders at base scale
        // (reference .lrcActiveZoom class removal snaps the font-size back).
        m_transitionTimer->stop();
        m_lineZoomProgress.fill(0.0);
        m_lineZoomTransition.fill(LineTransition{}, m_lines.size());
    }
    update();
}

void LyricRenderer::setFontWeightFont(bool /*on*/) {
    // No-op: write-only, the value no longer affects rendering (line-by-line
    // mode); retained for API compatibility with the host config sync.
}

void LyricRenderer::setFontWeightLine(bool on) {
    m_fontWeightLine = on;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setFontWeightExtended(bool on) {
    m_fontWeightExtended = on;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setUnplayColor(const QColor &c) {
    m_unplayColor = c;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setPlayedColor(const QColor &c) {
    m_playedColor = c;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setShadowColor(const QColor &c) {
    m_shadowColor = c;
    invalidateLineCaches();
    update();
}

void LyricRenderer::setShadowFontModeColor(const QColor & /*color*/) {
    // No-op: write-only, the value no longer affects rendering (line-by-line
    // mode; the stroke always uses m_shadowColor); retained for API
    // compatibility with the host config sync.
}

void LyricRenderer::setScrollAlign(bool top) {
    if (m_scrollAlignTop == top)
        return;
    m_scrollAlignTop = top;
    if (!m_userScrolling)
        scrollToActiveAnimated(kScrollAnimationMs);
}

void LyricRenderer::setDelayScroll(bool on) {
    m_delayScroll = on;
    if (!on)
        m_delayScrollTimer->stop();
}

void LyricRenderer::setUserScrolling(bool on) {
    if (m_userScrolling == on)
        return;
    m_userScrolling = on;
    if (on) {
        suspendAutoScroll();
    } else {
        rearmResumeTimer(); // 3 s later the auto-scroll re-centers the active
                            // line.
    }
}

void LyricRenderer::setInteractive(bool on) {
    if (m_interactive == on)
        return;
    m_interactive = on;
    if (on) {
        setCursor(Qt::OpenHandCursor);
    } else {
        if (m_dragging) {
            m_dragging = false;
            emit userInteractingChanged(false);
        }
        unsetCursor();
    }
}

void LyricRenderer::setPlaying(bool on) { m_playing = on; }

void LyricRenderer::resetScroll() {
    m_delayScrollTimer->stop();
    stopScrollAnimation();
    m_scrollOffset = 0;
    update();
}

void LyricRenderer::animateLineColors(int newActiveLine) {
    if (newActiveLine == m_activeLine)
        return; // Nothing changed (defensive: setActiveLine already guards).
    const int oldLine = m_activeLine;

    // Each line transitions from its CURRENT progress toward its target (CSS
    // semantics): the outgoing line fades played -> unplay and the incoming
    // line the other way, each over its own 600 ms tick. A line that flips
    // active status again mid-transition continues from where it is — the old
    // shared cross-fade snapped the outgoing line back to full played on every
    // change, which left a green trail behind during fast lyrics.
    if (oldLine >= 0)
        startLineTransition(m_lineColorTransition, m_lineColorProgress, oldLine,
                            0.0);
    if (newActiveLine >= 0)
        startLineTransition(m_lineColorTransition, m_lineColorProgress,
                            newActiveLine, 1.0);
    update();
}

QColor LyricRenderer::colorForLine(int lineIndex) const {
    const double p = (lineIndex >= 0 && lineIndex < m_lineColorProgress.size())
                         ? m_lineColorProgress.at(lineIndex)
                         : 0.0;
    return interpolateColor(m_unplayColor, m_playedColor, p);
}

void LyricRenderer::startZoomTransition(int newActiveLine) {
    if (!m_zoomActiveLrc) {
        // Zoom disabled: every line renders at base scale — jump there
        // instantly, no animation (reference .lrcActiveZoom class off).
        m_transitionTimer->stop();
        m_lineZoomProgress.fill(0.0);
        m_lineZoomTransition.fill(LineTransition{}, m_lines.size());
        update();
        return;
    }

    // Per-line zoom transitions, same CSS semantics as the color transitions:
    // a freshly active line grows 0 -> 1 while the outgoing line decays 1 -> 0
    // over its own 600 ms tick, and a line that loses active status
    // mid-transition shrinks from its CURRENT size — never snapping back to
    // the full 1.2x. When the zoom is toggled on for an already-active line
    // (newActiveLine == m_activeLine) that line just grows in from its current
    // progress.
    const bool sameLine = newActiveLine >= 0 && newActiveLine == m_activeLine;
    const int oldLine = sameLine ? -1 : m_activeLine;
    const int newLine = sameLine ? m_activeLine : newActiveLine;
    if (oldLine >= 0)
        startLineTransition(m_lineZoomTransition, m_lineZoomProgress, oldLine,
                            0.0);
    if (newLine >= 0)
        startLineTransition(m_lineZoomTransition, m_lineZoomProgress, newLine,
                            1.0);
    update();
}

void LyricRenderer::startLineTransition(QVector<LineTransition> &transitions,
                                        QVector<double> &progress, int line,
                                        double to) {
    if (line < 0 || line >= progress.size())
        return;
    LineTransition &t = transitions[line];
    const double from = progress.at(line);
    t.from = from;
    t.to = to;
    t.startMs = m_transitionClock.elapsed();
    t.active = true;
    if (qAbs(from - to) < 1e-6) {
        // Already at the target: no transition, no tick needed.
        t.active = false;
        return;
    }
    m_transitionTimer->start();
}

void LyricRenderer::stepTransitions() {
    bool anyActive = false;
    const qint64 now = m_transitionClock.elapsed();
    const auto step = [&](QVector<LineTransition> &transitions,
                          QVector<double> &progress) {
        for (int i = 0; i < transitions.size(); ++i) {
            LineTransition &t = transitions[i];
            if (!t.active)
                continue;
            const qreal u = qreal(now - t.startMs) / kTransitionAnimationMs;
            if (u >= 1.0) {
                progress[i] = t.to; // Settle exactly on the target.
                t.active = false;
                // The last mid-flight frame differs from the settled state:
                // the next paint re-rasterizes this line.
                if (i < m_lineCache.size())
                    m_lineCache[i].dirty = true;
            } else {
                progress[i] = t.from + (t.to - t.from) * easeOutCubic(u);
                anyActive = true;
            }
        }
    };
    step(m_lineColorTransition, m_lineColorProgress);
    step(m_lineZoomTransition, m_lineZoomProgress);
    if (!anyActive)
        m_transitionTimer->stop();
    update();
}

void LyricRenderer::stopScrollAnimation() {
    if (m_scrollTimer)
        m_scrollTimer->stop();
    m_scrollHasQueuedTarget = false;
}

void LyricRenderer::rebaseScroll(qreal target, int durationMs) {
    target = qBound<qreal>(0, target, maxScrollOffset());
    m_scrollStartOffset = m_scrollOffset;
    m_scrollTarget = target;
    m_scrollDurationMs = durationMs;
    m_scrollElapsedMs = 0;
    m_scrollHasQueuedTarget = false;
    m_scrollTimer->start();
}

void LyricRenderer::scrollTick() {
    m_scrollElapsedMs += kScrollStepMs;
    m_scrollOffset =
        easeInOutQuad(m_scrollElapsedMs, m_scrollStartOffset,
                      m_scrollTarget - m_scrollStartOffset, m_scrollDurationMs);
    update();
    if (m_scrollElapsedMs < m_scrollDurationMs)
        return;
    if (m_scrollHasQueuedTarget) {
        // A newer target arrived mid-flight: restart from the current offset
        // toward it (reference animateScroll's lx_scrollNextParams).
        rebaseScroll(m_scrollQueuedTarget, m_scrollQueuedDurationMs);
    } else {
        m_scrollTimer->stop();
        m_scrollOffset = m_scrollTarget; // Exact settle (last step eases).
        update();
    }
}

void LyricRenderer::paintEvent(QPaintEvent *) {
    if (m_lines.isEmpty())
        return;

    // A device-pixel-ratio change (e.g. moving between per-screen-DPR
    // displays) can arrive without a resizeEvent; the stale caches would
    // otherwise blit a 1x raster upscaled on a 2x window.
    if (!qFuzzyCompare(devicePixelRatioF(), m_cacheDpr)) {
        m_cacheDpr = devicePixelRatioF();
        invalidateLineCaches();
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // Cached-line blits land at fractional positions during zoom transitions
    // (the reflow shifts rows sub-pixel); the smooth hint keeps them
    // continuous, like Chromium's filtered layer compositing. Where positions
    // are whole pixels the blit stays an exact 1:1 copy; rows below a settled
    // 1.2x active line rest at fractional offsets and get the same slight
    // filtering the reference applies when it composites its layers.
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setOpacity(m_opacityPercent /
                       100.0); // Fresh painter: reset per paint.

    if (m_vertical)
        paintVertical(painter);
    else
        paintHorizontal(painter);
}

qreal LyricRenderer::mainZoomFactor(int lineIndex, double zoomOverride) const {
    if (!m_zoomActiveLrc)
        return 1.0;
    // Each line carries its own 0..1 progress (1.0 = fully zoomed), so the
    // incoming line grows while the outgoing line shrinks over the same
    // transition: 1.0x at progress 0, kActiveMainZoom at progress 1
    // (reference .lrcActiveZoom .line { font-size: 1.2em }). An explicit
    // zoomOverride (>= 0) measures the line at a fixed progress for the
    // scroll-target compensation.
    const qreal progress =
        (zoomOverride >= 0.0) ? zoomOverride
        : (lineIndex >= 0 && lineIndex < m_lineZoomProgress.size())
            ? m_lineZoomProgress.at(lineIndex)
            : 0.0;
    return 1.0 + (kActiveMainZoom - 1.0) * progress;
}

qreal LyricRenderer::extendedZoomFactor(int lineIndex,
                                        double zoomOverride) const {
    if (!m_zoomActiveLrc)
        return 1.0;
    // Extended lines sit at kExtendedScale; the active zoomed group grows them
    // to kActiveExtendedZoom (reference .extended 0.8em -> .lrcActiveZoom
    // .active .extended .94em). Same per-line progress as the main line; the
    // factor is relative to the base extended size (0.94em / 0.8em = 1.175
    // max). When the zoom is disabled the extended lines render at their base
    // 0.8x size, so the factor is 1.0.
    const qreal progress =
        (zoomOverride >= 0.0) ? zoomOverride
        : (lineIndex >= 0 && lineIndex < m_lineZoomProgress.size())
            ? m_lineZoomProgress.at(lineIndex)
            : 0.0;
    return (kExtendedScale +
            (kActiveExtendedZoom - kExtendedScale) * progress) /
           kExtendedScale;
}

QFont LyricRenderer::makeMainFont(int /*lineIndex*/,
                                  double /*zoomOverride*/) const {
    QFont font;
    if (!m_fontFamily.isEmpty())
        font.setFamily(m_fontFamily);
    // The font NEVER changes size: the active-line zoom is applied as a
    // painter transform (mainZoomFactor) so the transition rasterizes
    // fractional sizes. Qt rounds setPixelSize/setPointSizeF to whole pixels,
    // so the old qRound(m_fontSize * zoom) font stepped in ~4 discrete jumps
    // over the 600 ms transition — the stiffness this transform replaces.
    font.setPixelSize(m_fontSize);
    font.setBold(m_fontWeightLine); // Every line renders line-mode style.
    return font;
}

QFont LyricRenderer::makeExtendedFont(int /*lineIndex*/,
                                      double /*zoomOverride*/) const {
    QFont font;
    if (!m_fontFamily.isEmpty())
        font.setFamily(m_fontFamily);
    // Base extended size (reference .extended 0.8em); the active zoom applies
    // via the painter transform (extendedZoomFactor) — same rationale as
    // makeMainFont.
    font.setPixelSize(qRound(m_fontSize * kExtendedScale));
    font.setBold(m_fontWeightExtended);
    return font;
}

int LyricRenderer::textFlags() const {
    if (m_vertical)
        return Qt::AlignCenter; // Each vertical character fills its own cell.
    int flags = Qt::AlignVCenter;
    switch (m_align & Qt::AlignHorizontal_Mask) {
    case Qt::AlignLeft:
        flags |= Qt::AlignLeft;
        break;
    case Qt::AlignRight:
        flags |= Qt::AlignRight;
        break;
    case Qt::AlignHCenter:
    default:
        flags |= Qt::AlignHCenter;
        break;
    }
    return flags;
}

QString LyricRenderer::elideForWidth(const QString &text,
                                     const QFontMetrics &fm, int availableWidth,
                                     qreal scale) const {
    if (!m_ellipsis || text.isEmpty() || availableWidth <= 0)
        return text;
    // The SCALED text must fit the available width, so the elide happens in
    // base-font space (availableWidth / scale) — the same base font draws
    // scaled by the painter, so the ellipsis lands at the right place.
    if (fm.horizontalAdvance(text) * scale <= availableWidth)
        return text;
    return fm.elidedText(text, Qt::ElideRight, int(availableWidth / scale));
}

QString LyricRenderer::elideForHeight(const QString &text,
                                      const QFontMetrics &fm,
                                      int availableHeight, int letterSpacing,
                                      qreal scale) const {
    if (!m_ellipsis || text.isEmpty() || availableHeight <= 0)
        return text;
    if (columnHeight(text, fm, letterSpacing, scale) <= availableHeight)
        return text;
    // Qt 6.11 removed Qt::ElideBottom, so the vertical column is elided by
    // hand with the same semantics: drop trailing characters until the column
    // (ellipsis included) fits the available height, then append "…".
    const qreal step = fm.height() * scale + letterSpacing;
    const int maxChars = int(availableHeight / step);
    if (maxChars <= 1)
        return QStringLiteral("…");
    return text.left(maxChars - 1) + QStringLiteral("…");
}

qreal LyricRenderer::columnWidth(const QString &text, const QFontMetrics &fm,
                                 qreal scale) const {
    qreal widest = 0;
    for (const QChar ch : text)
        widest = qMax(widest, fm.horizontalAdvance(ch) * scale);
    return widest;
}

qreal LyricRenderer::columnHeight(const QString &text, const QFontMetrics &fm,
                                  int letterSpacing, qreal scale) const {
    if (text.isEmpty())
        return 0;
    const qreal step = fm.height() * scale + letterSpacing;
    return text.size() * step - letterSpacing; // N boxes, N-1 inter-char gaps.
}

LyricRenderer::VerticalGroupMetrics
LyricRenderer::measureVerticalGroup(const RenderLine &line, int lineIndex,
                                    int zoomOverrideLine,
                                    double zoomOverride) const {
    VerticalGroupMetrics m;
    const double mainZoom =
        (zoomOverrideLine == lineIndex) ? zoomOverride : -1.0;
    // Fractional zoom factors scale the base-font metrics continuously (the
    // fonts themselves stay at their base size; see makeMainFont), so a zoomed
    // group reflows at fractional sizes like the reference font-size
    // transition.
    const qreal mainScale = mainZoomFactor(lineIndex, mainZoom);
    const qreal extScale = extendedZoomFactor(lineIndex, mainZoom);
    m.mainFont = makeMainFont(lineIndex);
    m.extendedFont = makeExtendedFont(lineIndex);
    const QFontMetrics mainFm(m.mainFont);
    const QFontMetrics extendedFm(m.extendedFont);

    const int availableHeight = height();
    // Every line renders line-mode style, so the vertical line-mode
    // letter-spacing applies to all columns.
    const int letterSpacing = kVerticalLetterSpacing;

    // Wrap mode mirrors the reference .line-content overflow-wrap:break-word
    // (writing-mode: vertical-rl): text taller than the container wraps into
    // columns stacked right-to-left. Ellipsis mode clamps to one column with a
    // trailing "…" (reference .ellipsis -webkit-line-clamp:1).
    const QString mainText = stripWordTags(line.text);
    m.mainColumns =
        m_ellipsis
            ? QStringList{elideForHeight(mainText, mainFm, availableHeight,
                                         letterSpacing, mainScale)}
            : wrapForHeight(mainText, mainFm, availableHeight, letterSpacing,
                            mainScale);

    for (const QString &col : m.mainColumns) {
        m.groupWidth += columnWidth(col, mainFm, mainScale);
        m.groupHeight = qMax(
            m.groupHeight, columnHeight(col, mainFm, letterSpacing, mainScale));
    }
    // Wrapped columns advance by their glyph width plus the letter-spacing
    // (the reference separates columns with line-height leading; the port
    // uses the letter-spacing as the inter-column breathing room).
    if (m.mainColumns.size() > 1)
        m.groupWidth += letterSpacing * (m.mainColumns.size() - 1);

    for (const QString &rawExtended : line.extended) {
        const QString extText = stripWordTags(rawExtended);
        const QStringList extCols =
            m_ellipsis ? QStringList{elideForHeight(extText, extendedFm,
                                                    availableHeight,
                                                    letterSpacing, extScale)}
                       : wrapForHeight(extText, extendedFm, availableHeight,
                                       letterSpacing, extScale);
        m.extendedColumns << extCols;
        for (const QString &col : extCols) {
            m.groupWidth += columnWidth(col, extendedFm, extScale);
            m.groupHeight =
                qMax(m.groupHeight,
                     columnHeight(col, extendedFm, letterSpacing, extScale));
        }
        if (extCols.size() > 1)
            m.groupWidth += letterSpacing * (extCols.size() - 1);
    }
    if (!line.extended.isEmpty())
        m.groupWidth +=
            qRound(verticalExtendedGap(m_lineGap)) * line.extended.size();

    return m;
}

void LyricRenderer::paintHorizontal(QPainter &p) {
    const qreal gap = m_lineGap;
    const qreal lineWidth = width();

    // Measure each group (main + extended rows) so the block can be centered,
    // then shift the whole block by the scroll offset (content moves up as the
    // offset grows). Settled lines reuse the cached measure (keyed on the same
    // dirty/transitioning flags as the raster cache), so only the ≤2
    // transitioning lines re-wrap per 16 ms tick — with the raster cached, the
    // word-wrap pass is the dominant per-frame cost.
    qreal blockHeight = 0;
    if (m_lines.size() > 1)
        blockHeight += m_lineGap * (m_lines.size() - 1);
    for (int i = 0; i < m_lines.size(); ++i) {
        if (m_lineCache.at(i).dirty || isLineTransitioning(i))
            m_cachedGroupHeights[i] = measureLineGroupHeight(i);
        blockHeight += m_cachedGroupHeights.at(i);
    }
    // The block starts below the 80% leading spacer (reference .lyricSpace), so
    // the active line can always center — even the first/last lines. The
    // static no-lyrics placeholder centers the whole block in the viewport
    // instead (no active line to track).
    const qreal yOffset = m_centeredBlock
                              ? qMax<qreal>(0, (height() - blockHeight) / 2.0)
                              : qRound(kLyricSpaceRatio * height());

    qreal y = yOffset - m_scrollOffset;
    for (int i = 0; i < m_lines.size(); ++i) {
        const qreal h = m_cachedGroupHeights.at(i);
        const QRectF rect(0, y, lineWidth, h);
        // Settled lines blit their cached raster (1:1 copy — same pixels as
        // the direct raster it replaced); only lines with a mid-flight
        // zoom/color transition re-rasterize, so a transition frame costs one
        // blit per settled line plus ≤2 fresh rasters instead of a full-window
        // text + 32-pass-shadow raster every 16 ms tick.
        if (m_lineCache.at(i).dirty || isLineTransitioning(i))
            renderLineToCache(i, rect);
        p.drawPixmap(rect.topLeft() - QPointF(kCacheHaloPadPx, kCacheHaloPadPx),
                     m_lineCache.at(i).pixmap);
        y += h + gap;
    }
}

void LyricRenderer::paintVertical(QPainter &p) {
    const qreal gap = verticalGroupGap(m_lineGap);

    // Measure every column group so the whole block can be centered; scrolling
    // shifts the block left (content moves left as the offset grows). Settled
    // lines reuse the cached measure (same dirty key as the raster cache), so
    // only transitioning lines re-measure per 16 ms tick.
    QVector<VerticalGroupMetrics> liveMetrics(m_lines.size());
    qreal blockWidth = 0;
    if (m_lines.size() > 1)
        blockWidth += gap * (m_lines.size() - 1);
    for (int i = 0; i < m_lines.size(); ++i) {
        if (m_lineCache.at(i).dirty || isLineTransitioning(i)) {
            liveMetrics[i] = measureVerticalGroup(m_lines.at(i), i);
            m_cachedGroupWidths[i] = liveMetrics[i].groupWidth;
            m_cachedGroupHeights[i] = liveMetrics[i].groupHeight;
        }
        blockWidth += m_cachedGroupWidths.at(i);
    }

    // The block starts after the 80% leading spacer (reference .lyricSpace
    // width:80%), so the active column can always center — even the first/last.
    // The static no-lyrics placeholder centers the whole block instead.
    const qreal xOffset = m_centeredBlock
                              ? qMax<qreal>(0, (width() - blockWidth) / 2.0)
                              : qRound(kLyricSpaceRatio * width());

    // writing-mode: vertical-rl — blocks stack right-to-left, line 0 rightmost.
    qreal right = xOffset + blockWidth - m_scrollOffset;
    for (int i = 0; i < m_lines.size(); ++i) {
        const qreal groupWidth = m_cachedGroupWidths.at(i);
        const QRectF rect(right - groupWidth, 0, groupWidth,
                          m_cachedGroupHeights.at(i));
        if (m_lineCache.at(i).dirty || isLineTransitioning(i))
            renderLineToCache(i, rect, &liveMetrics.at(i));
        p.drawPixmap(rect.topLeft() - QPointF(kCacheHaloPadPx, kCacheHaloPadPx),
                     m_lineCache.at(i).pixmap);
        right -= groupWidth + gap;
    }
}

void LyricRenderer::invalidateLineCaches() {
    // Every line re-rasterizes and re-measures on the next paint (resize keeps
    // the pixmaps).
    m_lineCache.resize(m_lines.size());
    m_cachedGroupHeights.resize(m_lines.size());
    m_cachedGroupWidths.resize(m_lines.size());
    for (LineCache &cache : m_lineCache)
        cache.dirty = true;
}

bool LyricRenderer::isLineTransitioning(int lineIndex) const {
    if (lineIndex < 0 || lineIndex >= m_lineZoomProgress.size())
        return false;
    // A mid-flight zoom or color transition must re-rasterize every frame;
    // settled progress (exactly 0.0 or 1.0) lives in the cache as-is.
    const double zoom = m_lineZoomProgress.at(lineIndex);
    const double color = m_lineColorProgress.at(lineIndex);
    return (zoom > 0.0 && zoom < 1.0) || (color > 0.0 && color < 1.0);
}

void LyricRenderer::renderLineToCache(
    int lineIndex, const QRectF &rect,
    const VerticalGroupMetrics *verticalMetrics) {
    LineCache &cache = m_lineCache[lineIndex];
    // The pixmap lives in device pixels: a logical rect sized at the window's
    // device-pixel ratio, and drawn in logical coordinates (Qt scales the
    // painter) — so a 2x display gets a full-resolution cache. A transparent
    // kCacheHaloPadPx halo surrounds the group rect so the stroke passes that
    // draw beyond it (see kCacheHaloPadPx) survive the pixmap clip: the group
    // is drawn INSIDE the pixmap at (+pad, +pad) and the blit offsets the
    // pixmap by -pad, so the content lands exactly on the group rect.
    const qreal dpr = devicePixelRatioF();
    const int padDev = qCeil(kCacheHaloPadPx * dpr);
    const QSize deviceSize(qCeil(rect.width() * dpr) + 2 * padDev,
                           qCeil(rect.height() * dpr) + 2 * padDev);
    if (cache.pixmap.isNull() || cache.pixmap.size() != deviceSize ||
        !qFuzzyCompare(cache.pixmap.devicePixelRatio(), dpr)) {
        cache.pixmap = QPixmap(deviceSize);
        cache.pixmap.setDevicePixelRatio(dpr);
    }
    cache.pixmap.fill(Qt::transparent);
    QPainter cachePainter(&cache.pixmap);
    // Same hints as paintEvent minus the opacity: the widget-level opacity is
    // applied when the cache is blitted, so the pixmap always holds the
    // full-strength content.
    cachePainter.setRenderHint(QPainter::Antialiasing);
    const QRectF local(kCacheHaloPadPx, kCacheHaloPadPx, rect.width(),
                       rect.height());
    if (verticalMetrics)
        drawVerticalGroup(cachePainter, *verticalMetrics, lineIndex, local);
    else
        drawHorizontalGroup(cachePainter, m_lines.at(lineIndex), lineIndex,
                            local);
    cache.dirty = false;
}

void LyricRenderer::drawHorizontalGroup(QPainter &p, const RenderLine &line,
                                        int lineIndex, const QRectF &rect) {
    const QFont mainFont = makeMainFont(lineIndex);
    const QFont extendedFont = makeExtendedFont(lineIndex);
    const QFontMetrics mainFm(mainFont);
    const QFontMetrics extendedFm(extendedFont);
    // Fractional active-line zoom factors: the base fonts render scaled by the
    // painter (see drawTextWithStroke) and the row boxes advance by scaled
    // line heights, so the whole group grows continuously.
    const qreal mainScale = mainZoomFactor(lineIndex);
    const qreal extScale = extendedZoomFactor(lineIndex);
    const qreal extGap = horizontalExtendedGap(m_lineGap);

    // Line-by-line highlight: the whole active line switches to the played
    // color, inactive lines stay unplay; the 600 ms transition interpolates
    // both (reference .line-mode.active .font-lrc; .extended .font-lrc shares
    // the stroke). Horizontal line-mode strokes render the shadow color at
    // 51% alpha (reference stroke3 / RGB_Alpha_Shade(0.49, ...)); vertical
    // uses the full color (stroke4) — see drawVerticalGroup.
    const QColor fill = colorForLine(lineIndex);
    const QColor stroke = shadedShadowColor(m_shadowColor);

    if (m_ellipsis) {
        // Ellipsis mode: one single line with a trailing "…" (reference
        // .ellipsis .font-lrc { .mixin-ellipsis(1) }).
        const QString mainText = elideForWidth(stripWordTags(line.text), mainFm,
                                               int(rect.width()), mainScale);
        const QRectF mainRect(rect.x(), rect.y(), rect.width(),
                              mainFm.height() * mainScale);
        drawTextWithStroke(p, mainText, mainRect, mainFont, fill, stroke,
                           StrokeStyle::Stroke3, mainScale);

        qreal y = rect.y() + mainFm.height() * mainScale;
        for (const QString &rawExtended : line.extended) {
            y += extGap;
            const QString extText =
                elideForWidth(stripWordTags(rawExtended), extendedFm,
                              int(rect.width()), extScale);
            drawTextWithStroke(p, extText,
                               QRectF(rect.x(), y, rect.width(),
                                      extendedFm.height() * extScale),
                               extendedFont, fill, stroke, StrokeStyle::Stroke3,
                               extScale);
            y += extendedFm.height() * extScale;
        }
        return;
    }

    // Wrap mode (reference .line-content overflow-wrap:break-word — there is
    // no separate wrap toggle): every row paints at one line-height. A wrapped
    // block taller than the rect is still fully drawn; the painter clips,
    // matching the reference where the layout height simply grows.
    qreal y = rect.y();
    const QStringList mainRows = wrapForWidth(stripWordTags(line.text), mainFm,
                                              int(rect.width()), mainScale);
    for (const QString &row : mainRows) {
        drawTextWithStroke(
            p, row,
            QRectF(rect.x(), y, rect.width(), mainFm.height() * mainScale),
            mainFont, fill, stroke, StrokeStyle::Stroke3, mainScale);
        y += mainFm.height() * mainScale;
    }
    for (const QString &rawExtended : line.extended) {
        y += extGap;
        const QStringList extRows =
            wrapForWidth(stripWordTags(rawExtended), extendedFm,
                         int(rect.width()), extScale);
        for (const QString &row : extRows) {
            drawTextWithStroke(p, row,
                               QRectF(rect.x(), y, rect.width(),
                                      extendedFm.height() * extScale),
                               extendedFont, fill, stroke, StrokeStyle::Stroke3,
                               extScale);
            y += extendedFm.height() * extScale;
        }
    }
}

void LyricRenderer::drawVerticalGroup(QPainter &p,
                                      const VerticalGroupMetrics &m,
                                      int lineIndex, const QRectF &rect) {
    const QFontMetrics mainFm(m.mainFont);
    const QFontMetrics extendedFm(m.extendedFont);
    const int letterSpacing =
        kVerticalLetterSpacing; // Every line renders line-mode style.
    const qreal extGap = verticalExtendedGap(m_lineGap);
    // Fractional active-line zoom factors (see drawHorizontalGroup).
    const qreal mainScale = mainZoomFactor(lineIndex);
    const qreal extScale = extendedZoomFactor(lineIndex);

    // Line-by-line highlight: the whole active column group switches to the
    // played color, inactive groups stay unplay (color transition included).
    // Vertical strokes use the FULL shadow color at ~1 px offsets (reference
    // stroke4), unlike the horizontal 51%-alpha shade (stroke3).
    const QColor fill = colorForLine(lineIndex);

    // Columns are drawn rightmost-first: in the wrapped flow (writing-mode
    // vertical-rl) the first column of each text sits at the right edge and
    // wrapped columns continue to the left, separated by the letter-spacing.
    // Extended groups sit left of the main group, separated by the extended
    // gap (reference .extended { margin-right: ... }).
    qreal right = rect.right();
    auto drawColumns = [&](const QStringList &cols, const QFont &font,
                           const QFontMetrics &fm, qreal scale) {
        for (int i = 0; i < cols.size(); ++i) {
            const qreal colWidth = columnWidth(cols.at(i), fm, scale);
            const QRectF colRect(right - colWidth, rect.top(), colWidth,
                                 m.groupHeight);
            drawVerticalText(p, cols.at(i), colRect, font, fill, m_shadowColor,
                             StrokeStyle::Stroke4, letterSpacing, scale);
            right = colRect.left();
            if (i < cols.size() - 1)
                right -= letterSpacing;
        }
    };
    drawColumns(m.mainColumns, m.mainFont, mainFm, mainScale);
    for (const QStringList &extCols : m.extendedColumns) {
        right -= extGap;
        drawColumns(extCols, m.extendedFont, extendedFm, extScale);
    }
}

void LyricRenderer::drawVerticalText(QPainter &p, const QString &text,
                                     const QRectF &columnRect,
                                     const QFont &font, const QColor &fill,
                                     const QColor &stroke, StrokeStyle style,
                                     int letterSpacing, qreal scale) {
    if (text.isEmpty())
        return;
    const QFontMetrics fm(font);
    // Each character owns one cell: the glyph renders base-size, scaled by the
    // painter about its cell center, and the cells advance by the scaled line
    // box (the letter-spacing itself stays fixed — CSS letter-spacing does not
    // zoom with the font).
    const qreal cellHeight = fm.height() * scale;
    const qreal step = cellHeight + letterSpacing;
    qreal y = columnRect.y();
    for (const QChar ch : text) {
        drawTextWithStroke(
            p, QString(ch),
            QRectF(columnRect.x(), y, columnRect.width(), cellHeight), font,
            fill, stroke, style, scale);
        y += step;
    }
}

void LyricRenderer::drawTextWithStroke(QPainter &p, const QString &text,
                                       const QRectF &rect, const QFont &font,
                                       const QColor &fill, const QColor &stroke,
                                       StrokeStyle style, qreal scale) {
    if (text.isEmpty())
        return;

    p.setFont(font);
    const int flags = textFlags();

    // Active-line zoom as a painter transform: the glyphs rasterize at
    // FRACTIONAL sizes (Qt rounds font sizes to whole pixels, so resizing the
    // font made the transition step in ~4 discrete jumps). The transform also
    // scales the em-based stroke offsets correctly (CSS em = the element's own
    // font-size); the stroke4 px offsets (±1 px) scale along — a ≤0.2 px
    // deviation at the 1.2x max, visually identical.
    //
    // The horizontal anchor follows the text alignment, mirroring CSS where
    // the growing text box reflows around the aligned edge: left-aligned text
    // grows rightward from the left edge, right-aligned grows leftward from
    // the right edge (so it never overflows the widget), centered grows from
    // the middle. Vertical cells stay anchored at their own center (each
    // character fills its cell and the group itself is centered).
    const bool scaled = qAbs(scale - 1.0) > 1e-9;
    if (scaled) {
        QPointF anchor = rect.center();
        if (!m_vertical) {
            switch (m_align & Qt::AlignHorizontal_Mask) {
            case Qt::AlignLeft:
                anchor.setX(rect.left());
                break;
            case Qt::AlignRight:
                anchor.setX(rect.right());
                break;
            default:
                break; // Qt::AlignHCenter — anchored at the center.
            }
        }
        p.save();
        p.translate(anchor);
        p.scale(scale, scale);
        p.translate(-anchor);
    }

    // Replicate the reference CSS text-shadow stack (stroke3/stroke4): every
    // table entry is one zero-blur glyph copy drawn under the text fill, and
    // duplicate offsets are intentional alpha passes. Em offsets scale with
    // the rendering font's pixel size (CSS em = the element's own font-size,
    // active-line zoom and the 0.8x extended scale included); px offsets are
    // fixed device pixels.
    const qreal em = font.pixelSize();
    const StrokeOffset *offsets = (style == StrokeStyle::Stroke3)
                                      ? kStroke3Offsets.data()
                                      : kStroke4Offsets.data();
    const std::size_t passCount = (style == StrokeStyle::Stroke3)
                                      ? kStroke3Offsets.size()
                                      : kStroke4Offsets.size();

    p.setPen(stroke);
    p.setBrush(stroke);
    for (std::size_t i = 0; i < passCount; ++i) {
        const StrokeOffset &offset = offsets[i];
        const qreal dx = offset.xIsEm ? offset.x * em : offset.x;
        const qreal dy = offset.yIsEm ? offset.y * em : offset.y;
        p.drawText(rect.translated(dx, dy), flags, text);
    }

    p.setPen(fill);
    p.setBrush(fill);
    p.drawText(rect, flags, text);

    if (scaled)
        p.restore();
}

QVector<QPointF> LyricRenderer::strokeOffsetsPx(StrokeStyle style, qreal emPx) {
    const StrokeOffset *offsets = (style == StrokeStyle::Stroke3)
                                      ? kStroke3Offsets.data()
                                      : kStroke4Offsets.data();
    const std::size_t count = (style == StrokeStyle::Stroke3)
                                  ? kStroke3Offsets.size()
                                  : kStroke4Offsets.size();
    QVector<QPointF> result;
    result.reserve(int(count));
    for (std::size_t i = 0; i < count; ++i) {
        const StrokeOffset &offset = offsets[i];
        result.append(QPointF(offset.xIsEm ? offset.x * emPx : offset.x,
                              offset.yIsEm ? offset.y * emPx : offset.y));
    }
    return result;
}

LyricRenderer::HorizontalLayout
LyricRenderer::measureHorizontal(int zoomOverrideLine,
                                 double zoomOverride) const {
    HorizontalLayout layout;
    layout.groupHeights.resize(m_lines.size());
    for (int i = 0; i < m_lines.size(); ++i) {
        const double zoom = (i == zoomOverrideLine) ? zoomOverride : -1.0;
        layout.groupHeights[i] = measureLineGroupHeight(i, zoom);
        layout.blockHeight += layout.groupHeights[i];
    }
    if (m_lines.size() > 1)
        layout.blockHeight += m_lineGap * (m_lines.size() - 1);
    return layout;
}

qreal LyricRenderer::measureLineGroupHeight(int lineIndex,
                                            double zoomOverride) const {
    const RenderLine &line = m_lines.at(lineIndex);
    // Fractional zoom factors: heights are base metrics × factor, so the
    // block reflows continuously during the zoom transition (the old
    // rounded-font heights stepped in discrete jumps).
    const qreal mainScale = mainZoomFactor(lineIndex, zoomOverride);
    const qreal extScale = extendedZoomFactor(lineIndex, zoomOverride);
    const QFontMetrics mainFm(makeMainFont(lineIndex));
    const QFontMetrics extendedFm(makeExtendedFont(lineIndex));
    // The same width the paint pass draws into (the widget's own width), so a
    // wrapped line measures exactly as tall as it paints.
    const int wrapWidth = width();
    qreal groupHeight = 0;
    if (m_ellipsis) {
        groupHeight += mainFm.height() * mainScale;
        if (!line.extended.isEmpty())
            groupHeight +=
                line.extended.size() * (horizontalExtendedGap(m_lineGap) +
                                        extendedFm.height() * extScale);
    } else {
        groupHeight +=
            wrapForWidth(stripWordTags(line.text), mainFm, wrapWidth, mainScale)
                .size() *
            mainFm.height() * mainScale;
        for (const QString &rawExtended : line.extended)
            groupHeight += horizontalExtendedGap(m_lineGap) +
                           wrapForWidth(stripWordTags(rawExtended), extendedFm,
                                        wrapWidth, extScale)
                                   .size() *
                               extendedFm.height() * extScale;
    }
    return groupHeight;
}

QVector<LyricRenderer::VerticalGroupMetrics>
LyricRenderer::measureAllVerticalGroups(int zoomOverrideLine,
                                        double zoomOverride) const {
    QVector<VerticalGroupMetrics> metrics(m_lines.size());
    for (int i = 0; i < m_lines.size(); ++i)
        metrics[i] = measureVerticalGroup(m_lines.at(i), i, zoomOverrideLine,
                                          zoomOverride);
    return metrics;
}

qreal LyricRenderer::verticalBlockWidth(
    const QVector<VerticalGroupMetrics> &metrics) const {
    qreal blockWidth = 0;
    for (const VerticalGroupMetrics &m : metrics)
        blockWidth += m.groupWidth;
    if (metrics.size() > 1)
        blockWidth += verticalGroupGap(m_lineGap) * (metrics.size() - 1);
    return blockWidth;
}

qreal LyricRenderer::maxScrollOffset() const {
    if (m_lines.isEmpty() || m_centeredBlock)
        return 0; // Static placeholder: the block is centered, nothing scrolls.
    const qreal blockExtent =
        m_vertical ? verticalBlockWidth(measureAllVerticalGroups())
                   : measureHorizontal().blockHeight;
    const int viewportExtent = m_vertical ? width() : height();
    // The block sits below an 80% leading spacer with an 80% trailing spacer
    // (reference .lyricSpace), so the scrollable extent is 1.6 * viewport +
    // block; the usable range holds kLyricSpaceInnerFactor of viewport
    // headroom beyond the block — always > 0 while lines exist.
    return qMax<qreal>(0,
                       kLyricSpaceInnerFactor * viewportExtent + blockExtent);
}

qreal LyricRenderer::autoScrollTarget() const {
    if (m_lines.isEmpty() || m_activeLine < 0 || m_activeLine >= m_lines.size())
        return 0;

    if (m_vertical) {
        const QVector<VerticalGroupMetrics> metrics =
            measureAllVerticalGroups();
        const qreal blockWidth = verticalBlockWidth(metrics);
        // Leading 80% spacer: the block's left edge starts here, so even the
        // last (leftmost) column can center (reference .lyricSpace width:80%).
        const qreal xOffset = qRound(kLyricSpaceRatio * width());
        const qreal gap = verticalGroupGap(m_lineGap);

        // Base left edge of the active group in the centered layout (line 0 is
        // rightmost, so scan right-to-left).
        qreal right = xOffset + blockWidth;
        qreal baseLeft = 0;
        for (int i = 0; i <= m_activeLine; ++i) {
            baseLeft = right - metrics.at(i).groupWidth;
            right -= metrics.at(i).groupWidth + gap;
        }

        // 'top' alignment pins the active column 2 px from the far/right
        // content edge (reference getOffsetTop: contentWidth - lineWidth - 2
        // on the negative-scroll axis); center keeps it horizontally centered.
        const qreal groupExtent = metrics.at(m_activeLine).groupWidth;
        const qreal target = m_scrollAlignTop ? (width() - groupExtent - 2)
                                              : (width() - groupExtent) / 2.0;
        return qBound<qreal>(0, baseLeft - target, maxScrollOffset());
    }

    const HorizontalLayout layout = measureHorizontal();
    // Reference isComputeHeight compensation (handleScrollLrc's
    // prevLineHeight): with zoom + immediate scroll the target is computed
    // while the outgoing line still renders at its full zoom, so the active
    // line sits (zoomed - base) lower than it settles; the reference subtracts
    // that delta for forward steps. With the delayed scroll the transition has
    // already settled before the scroll starts, so no compensation applies.
    // (Vertical mode needs none: its right-anchored layout keeps the active
    // column's base position fixed while the outgoing column's zoom decays.)
    qreal zoomCompensation = 0;
    if (m_zoomActiveLrc && !m_delayScroll && m_prevActiveLine >= 0 &&
        m_prevActiveLine < m_activeLine) {
        const qreal baseHeight = measureHorizontal(m_prevActiveLine, 0.0)
                                     .groupHeights.at(m_prevActiveLine);
        zoomCompensation =
            layout.groupHeights.at(m_prevActiveLine) - baseHeight;
    }

    // Leading 80% spacer: the block's top edge starts here, so even the last
    // line can center (reference .lyricSpace height:80%).
    const qreal yOffset = qRound(kLyricSpaceRatio * height());
    qreal baseTop = yOffset;
    for (int i = 0; i < m_activeLine; ++i)
        baseTop += layout.groupHeights.at(i) + m_lineGap;

    const qreal groupExtent = layout.groupHeights.at(m_activeLine);
    const qreal target = m_scrollAlignTop ? 0 : (height() - groupExtent) / 2.0;
    return qBound<qreal>(0, baseTop - zoomCompensation - target,
                         maxScrollOffset());
}

void LyricRenderer::scrollToActiveAnimated(int durationMs) {
    m_delayScrollTimer->stop();

    const qreal target = autoScrollTarget();
    if (durationMs <= 0 || qAbs(target - m_scrollOffset) < 0.5) {
        stopScrollAnimation();
        m_scrollOffset = target;
        update();
        return;
    }
    if (m_scrollTimer->isActive()) {
        // A new target mid-flight (reference handleScrollY): past 75% of the
        // current run, restart from the current offset toward the new target;
        // before that, queue it — the running animation keeps easing, so the
        // view never stalls while line changes outpace the animation (the old
        // code restarted the full ease from zero on every change and crawled
        // behind fast lyrics).
        if (m_scrollElapsedMs > kScrollRetargetThreshold * m_scrollDurationMs) {
            rebaseScroll(target, durationMs);
        } else {
            m_scrollHasQueuedTarget = true;
            m_scrollQueuedTarget = target; // Latest wins (lx_scrollNextParams).
            m_scrollQueuedDurationMs = durationMs;
        }
        return;
    }
    rebaseScroll(target, durationMs);
}

void LyricRenderer::suspendAutoScroll() {
    m_userScrolling = true;
    stopScrollAnimation();
    m_delayScrollTimer->stop();
}

void LyricRenderer::rearmResumeTimer() { m_resumeTimer->start(); }

void LyricRenderer::startDelayScrollTimer() {
    // One pending roll per burst, not one per line change: the timer is only
    // armed if no roll is already waiting, and its handler retargets to the
    // CURRENT active line when it fires (scrollToActiveAnimated recomputes the
    // target). Re-arming on every consecutive change would freeze the view for
    // the whole duration of a fast section and then roll a long distance in
    // one jump — the reference's stacked scrollLine timeouts roll instead.
    if (!m_delayScrollTimer->isActive())
        m_delayScrollTimer->start();
}

void LyricRenderer::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_lines.isEmpty())
        return;
    // Width changes re-wrap/elide every line and the device-pixel ratio may
    // change on a display move — cached rasters are stale.
    invalidateLineCaches();
    // The block re-centers on resize; clamp any manual offset to the new range
    // and, when not mid-interaction, keep the active line at its target —
    // animated, so a resize stream retargets smoothly instead of jumping.
    m_scrollOffset = qBound<qreal>(0, m_scrollOffset, maxScrollOffset());
    if (!m_userScrolling)
        scrollToActiveAnimated(kScrollAnimationMs);
    update();
}

void LyricRenderer::wheelEvent(QWheelEvent *event) {
    if (!m_interactive) {
        QWidget::wheelEvent(event);
        return;
    }
    const int deltaY = event->angleDelta().y();
    if (deltaY != 0) {
        suspendAutoScroll();
        // Horizontal: wheel down scrolls forward (offset grows, matching the
        // reference scrollTop += deltaY). Vertical-rl scrolls left instead, so
        // the sign is flipped (reference scrollLeft -= deltaY).
        const qreal delta = m_vertical ? -qreal(deltaY) : qreal(deltaY);
        m_scrollOffset =
            qBound<qreal>(0, m_scrollOffset + delta, maxScrollOffset());
        update();
        rearmResumeTimer();
    }
    event->accept();
}

void LyricRenderer::mousePressEvent(QMouseEvent *event) {
    if (!m_interactive || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_dragStartPos = event->position().toPoint();
    m_dragStartOffset = m_scrollOffset;
    suspendAutoScroll();
    setCursor(Qt::ClosedHandCursor);
    emit userInteractingChanged(true);
    event->accept();
}

void LyricRenderer::mouseMoveEvent(QMouseEvent *event) {
    if (!m_interactive || !m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    // Grab the content: the offset follows the pointer, clamped to its range.
    const QPoint delta = m_dragStartPos - event->position().toPoint();
    const qreal d = m_vertical ? delta.x() : delta.y();
    m_scrollOffset = qBound<qreal>(0, m_dragStartOffset + d, maxScrollOffset());
    update();
    rearmResumeTimer();
    event->accept();
}

void LyricRenderer::mouseReleaseEvent(QMouseEvent *event) {
    if (!m_interactive || !m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
    emit userInteractingChanged(false);
    rearmResumeTimer(); // Auto-scroll resumes 3 s after the last interaction.
    event->accept();
}
