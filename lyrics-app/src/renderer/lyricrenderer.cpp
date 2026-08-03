/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "renderer/lyricrenderer.h"

#include <QEasingCurve>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QRectF>
#include <QResizeEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <cmath>

namespace {

// Vertical line-mode letter-spacing (CSS .line-content.line-mode, vertical).
constexpr int kVerticalLetterSpacing = 5;
// Scroll timing (reference useLyric.js): 3 s of manual scrolling before the
// auto-scroll resumes; a delayed scroll waits 600 ms then animates over
// 600 ms (handleScrollLrc(600)), every other auto-scroll animates over 300 ms
// (handleScrollLrc()'s default).
constexpr int kResumeDelayMs = 3000;
constexpr int kDelayScrollMs = 600;         // pre-delay before a delayed scroll
constexpr int kDelayScrollAnimationMs = 600; // duration of a delayed scroll
constexpr int kScrollAnimationMs = 300;     // duration of a normal auto-scroll
// Active-line zoom factors (reference .lrcActiveZoom).
constexpr qreal kActiveMainZoom = 1.2;
constexpr qreal kActiveExtendedZoom = 0.94;
// Zoom transition duration (reference .line-mode .font-lrc transition:
// font-size .6s ease).
constexpr int kZoomAnimationMs = 600;
// Color transition duration (reference .line-mode .font-lrc transition:
// color .6s ease).
constexpr int kColorAnimationMs = 600;
// Extended lines render at 0.8x the main size (reference .extended).
constexpr qreal kExtendedScale = 0.8;
// Every line uses the 4 px shadow stroke (reference .line-mode .font-lrc,
// .extended .font-lrc stroke3); the 1 px font-mode variant is gone.
constexpr int kLineStrokeWidth = 4;
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

// Word-level karaoke tags are stripped; display is line-by-line per the
// original lx-music design. Removes every <digits,digits> sequence (JS
// timeRxpAll: /<(\d+),(\d+)>/g); extended lines arrive pre-tagged too.
const QRegularExpression kKaraokeTagRxp(QStringLiteral("<\\d+,\\d+>"));

QString stripWordTags(QString text)
{
    text.remove(kKaraokeTagRxp);
    return text;
}

// Horizontal gap formulas from the reference --line-extended-gap / --line-gap.
qreal horizontalExtendedGap(int lineGap)
{
    return lineGap / 3.0;
}

qreal verticalGroupGap(int lineGap)
{
    return std::ceil(lineGap * 1.06);
}

qreal verticalExtendedGap(int lineGap)
{
    return std::ceil(lineGap * 1.06 / 8.0);
}

// Line-by-line highlight color: linear interpolation between the unplay and
// played colors over the line's 0..1 color progress (per-channel qRound lerp,
// mirroring the CSS color transition between --color-lyric-unplay and
// --color-lyric-played).
QColor interpolateColor(const QColor& unplay, const QColor& played, double p)
{
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
// character.
QStringList breakLongWord(const QString& word, const QFontMetrics& fm, int width)
{
    QStringList chunks;
    QString chunk;
    qreal chunkWidth = 0;
    for (const QChar ch : word) {
        const qreal chWidth = fm.horizontalAdvance(ch);
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
QStringList wrapForWidth(const QString& text, const QFontMetrics& fm, int width)
{
    if (width <= 0 || text.isEmpty() || text.trimmed().isEmpty())
        return { text };

    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const qreal spaceWidth = fm.horizontalAdvance(QLatin1Char(' '));
    QStringList rows;
    QString row;
    qreal rowWidth = 0;

    auto startRow = [&](const QString& first) {
        row = first;
        rowWidth = fm.horizontalAdvance(first);
    };
    auto commitRow = [&]() {
        if (!row.isEmpty()) {
            rows << row;
            row.clear();
            rowWidth = 0;
        }
    };
    auto takeWord = [&](const QString& word) {
        const QStringList chunks = breakLongWord(word, fm, width);
        startRow(chunks.first());
        for (int i = 1; i < chunks.size(); ++i) {
            commitRow();
            startRow(chunks.at(i));
        }
    };

    for (const QString& word : words) {
        const qreal wordWidth = fm.horizontalAdvance(word);
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

} // namespace

LyricRenderer::LyricRenderer(QWidget* parent)
    : QWidget(parent)
{
    // Own the lyric area: expand into whatever space the layout leaves and
    // never collapse below one visible line (default Preferred policy + an
    // invalid default sizeHint lets a QVBoxLayout shrink this widget to ~0).
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(kMinimumHeight);

    // Auto-scroll resumes 3 s after the last wheel/drag interaction (reference
    // startLyricScrollTimeout); single-shot and re-armed on every interaction.
    // The reference gates the resume on isPlay; the renderer has no playing
    // signal, so it re-centers whenever the timer fires (documented deviation).
    m_resumeTimer = new QTimer(this);
    m_resumeTimer->setSingleShot(true);
    m_resumeTimer->setInterval(kResumeDelayMs);
    connect(m_resumeTimer, &QTimer::timeout, this, [this] {
        m_userScrolling = false;
        scrollToActiveAnimated(kScrollAnimationMs);
    });

    // isDelayScroll: the active-line change waits 600 ms, then scrolls
    // smoothly (reference scrollLine's setTimeout + handleScrollLrc(600)).
    m_delayScrollTimer = new QTimer(this);
    m_delayScrollTimer->setSingleShot(true);
    m_delayScrollTimer->setInterval(kDelayScrollMs);
    connect(m_delayScrollTimer, &QTimer::timeout, this, [this] {
        if (m_userScrolling)
            return; // User grabbed the lyrics meanwhile: leave it to the resume timer.
        scrollToActiveAnimated(kDelayScrollAnimationMs);
    });

    // Every auto-scroll is animated with InOutQuad (reference handleScrollY's
    // easeInOutQuad stepping). Restarting from the current value mid-flight is
    // smooth: stop() keeps the last interpolated value, which m_scrollOffset
    // already holds.
    m_scrollAnimation = new QVariantAnimation(this);
    m_scrollAnimation->setEasingCurve(QEasingCurve(QEasingCurve::InOutQuad));
    connect(m_scrollAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_scrollOffset = value.toReal();
                update();
            });

    // Active-line color transition: one 600 ms OutCubic animation drives the
    // shared 0..1 tick, cross-fading the outgoing line down and the incoming
    // line up (reference .line-mode .font-lrc { transition: color
    // @transition-slow }).
    m_colorAnim = new QVariantAnimation(this);
    m_colorAnim->setDuration(kColorAnimationMs);
    m_colorAnim->setEasingCurve(QEasingCurve(QEasingCurve::OutCubic));
    connect(m_colorAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                const double t = (value.toDouble() - m_colorAnimFrom)
                    / (m_colorAnimTo - m_colorAnimFrom);
                if (m_colorAnimOldLine >= 0 && m_colorAnimOldLine < m_lineColorProgress.size())
                    m_lineColorProgress[m_colorAnimOldLine] = 1.0 - t;
                if (m_colorAnimNewLine >= 0 && m_colorAnimNewLine < m_lineColorProgress.size())
                    m_lineColorProgress[m_colorAnimNewLine] = t;
                update();
            });

    // Active-line zoom transition: the reference .lrcActiveZoom CSS transitions
    // font-size over 0.6s ease; one shared OutCubic animation eases the
    // incoming line's progress 0..1 and the outgoing line's 1..0, repainting on
    // every tick.
    m_zoomAnim = new QVariantAnimation(this);
    m_zoomAnim->setDuration(kZoomAnimationMs);
    m_zoomAnim->setEasingCurve(QEasingCurve(QEasingCurve::OutCubic));
    connect(m_zoomAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                const double t = value.toDouble();
                if (m_zoomOldLine >= 0 && m_zoomOldLine < m_lineZoomProgress.size())
                    m_lineZoomProgress[m_zoomOldLine] = 1.0 - t;
                if (m_zoomNewLine >= 0 && m_zoomNewLine < m_lineZoomProgress.size())
                    m_lineZoomProgress[m_zoomNewLine] = t;
                update();
            });
}

QSize LyricRenderer::sizeHint() const
{
    return kSizeHint;
}

void LyricRenderer::setLines(const QVector<RenderLine>& lines)
{
    m_lines = lines;
    // Rebuild the per-line color/zoom progress with the lines: everything
    // starts unplayed and unzoomed except the current active line, which snaps
    // to its settled state — a lyric change does not animate.
    m_lineColorProgress.fill(0.0, m_lines.size());
    m_lineZoomProgress.fill(0.0, m_lines.size());
    if (m_activeLine >= 0 && m_activeLine < m_lines.size()) {
        m_lineColorProgress[m_activeLine] = 1.0;
        m_lineZoomProgress[m_activeLine] = 1.0;
    }
    if (m_colorAnim)
        m_colorAnim->stop();
    if (m_zoomAnim)
        m_zoomAnim->stop();
    resetScroll();
    if (!m_userScrolling && m_activeLine >= 0 && m_activeLine < m_lines.size())
        scrollToActiveInstant();
    update();
}

void LyricRenderer::setActiveLine(int index)
{
    // Parse at the boundary: out-of-range indexes mean "no active line".
    const int clamped = (index >= 0 && index < m_lines.size()) ? index : -1;
    if (m_activeLine == clamped)
        return;
    const int oldLine = m_activeLine;

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

void LyricRenderer::setVertical(bool on)
{
    m_vertical = on;
    update();
}

void LyricRenderer::setAlign(Qt::Alignment align)
{
    m_align = align;
    update();
}

void LyricRenderer::setFontFamily(const QString& family)
{
    m_fontFamily = family;
    update();
}

void LyricRenderer::setFontSize(int px)
{
    m_fontSize = qBound(10, px, 80);
    update();
}

void LyricRenderer::setLineGap(int px)
{
    m_lineGap = qBound(0, px, 25);
    update();
}

void LyricRenderer::setOpacityPercent(int percent)
{
    m_opacityPercent = qBound(6, percent, 100);
    update();
}

void LyricRenderer::setEllipsis(bool on)
{
    m_ellipsis = on;
    update();
}

void LyricRenderer::setZoomActiveLrc(bool on)
{
    if (m_zoomActiveLrc == on)
        return;
    m_zoomActiveLrc = on;
    if (on) {
        // Toggled on with a line active: grow it in over the transition
        // (reference class add -> CSS font-size transition).
        if (m_activeLine >= 0)
            startZoomTransition(m_activeLine);
    } else {
        // Toggled off: no transition — every line renders at base scale
        // (reference .lrcActiveZoom class removal snaps the font-size back).
        m_zoomAnim->stop();
        m_lineZoomProgress.fill(0.0);
    }
    update();
}

void LyricRenderer::setFontWeightFont(bool /*on*/)
{
    // No-op: write-only, the value no longer affects rendering (line-by-line
    // mode); retained for API compatibility with the host config sync.
}

void LyricRenderer::setFontWeightLine(bool on)
{
    m_fontWeightLine = on;
    update();
}

void LyricRenderer::setFontWeightExtended(bool on)
{
    m_fontWeightExtended = on;
    update();
}

void LyricRenderer::setUnplayColor(const QColor& c)
{
    m_unplayColor = c;
    update();
}

void LyricRenderer::setPlayedColor(const QColor& c)
{
    m_playedColor = c;
    update();
}

void LyricRenderer::setShadowColor(const QColor& c)
{
    m_shadowColor = c;
    update();
}

void LyricRenderer::setShadowFontModeColor(const QColor& /*color*/)
{
    // No-op: write-only, the value no longer affects rendering (line-by-line
    // mode; the stroke always uses m_shadowColor); retained for API
    // compatibility with the host config sync.
}

void LyricRenderer::setScrollAlign(bool top)
{
    if (m_scrollAlignTop == top)
        return;
    m_scrollAlignTop = top;
    if (!m_userScrolling)
        scrollToActiveAnimated(kScrollAnimationMs);
}

void LyricRenderer::setDelayScroll(bool on)
{
    m_delayScroll = on;
    if (!on)
        m_delayScrollTimer->stop();
}

void LyricRenderer::setUserScrolling(bool on)
{
    if (m_userScrolling == on)
        return;
    m_userScrolling = on;
    if (on) {
        suspendAutoScroll();
    } else {
        rearmResumeTimer(); // 3 s later the auto-scroll re-centers the active line.
    }
}

void LyricRenderer::setInteractive(bool on)
{
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

void LyricRenderer::resetScroll()
{
    m_delayScrollTimer->stop();
    if (m_scrollAnimation)
        m_scrollAnimation->stop();
    m_scrollOffset = 0;
    update();
}

void LyricRenderer::animateLineColors(int newActiveLine)
{
    if (newActiveLine == m_activeLine)
        return; // Nothing changed (defensive: setActiveLine already guards).
    const int oldLine = m_activeLine;

    // Cross-fade the outgoing line from played back to unplay and the incoming
    // line the other way, over one shared 0..1 tick (reference .line-mode
    // .font-lrc { transition: color @transition-slow }).
    if (oldLine >= 0 && oldLine < m_lineColorProgress.size())
        m_lineColorProgress[oldLine] = 1.0;
    if (newActiveLine >= 0 && newActiveLine < m_lineColorProgress.size())
        m_lineColorProgress[newActiveLine] = 0.0;

    m_colorAnimOldLine = oldLine;
    m_colorAnimNewLine = newActiveLine;
    m_colorAnimFrom = 0.0;
    m_colorAnimTo = 1.0;
    m_colorAnim->stop();
    m_colorAnim->setStartValue(m_colorAnimFrom);
    m_colorAnim->setEndValue(m_colorAnimTo);
    m_colorAnim->start();
    update();
}

QColor LyricRenderer::colorForLine(int lineIndex) const
{
    const double p = (lineIndex >= 0 && lineIndex < m_lineColorProgress.size())
        ? m_lineColorProgress.at(lineIndex)
        : 0.0;
    return interpolateColor(m_unplayColor, m_playedColor, p);
}

void LyricRenderer::startZoomTransition(int newActiveLine)
{
    if (!m_zoomActiveLrc) {
        // Zoom disabled: every line renders at base scale — jump there
        // instantly, no animation (reference .lrcActiveZoom class off).
        m_zoomAnim->stop();
        m_lineZoomProgress.fill(0.0);
        update();
        return;
    }

    // The zoom progress is per line. A freshly active line grows 0 -> 1 while
    // the outgoing line decays 1 -> 0 over one shared 600 ms tick; when the
    // zoom is toggled on for an already-active line (newActiveLine ==
    // m_activeLine) that line just grows in from base. Restarting from the
    // 0/1 snap each time is safe in Qt 6.11.1, the same stop/start pattern as
    // the Task G scroll animation.
    const bool sameLine = newActiveLine >= 0 && newActiveLine == m_activeLine;
    const int oldLine = sameLine ? -1 : m_activeLine;
    const int newLine = sameLine ? m_activeLine : newActiveLine;
    if (oldLine >= 0 && oldLine < m_lineZoomProgress.size())
        m_lineZoomProgress[oldLine] = 1.0;
    if (newLine >= 0 && newLine < m_lineZoomProgress.size())
        m_lineZoomProgress[newLine] = 0.0;
    if (oldLine < 0 && newLine < 0) {
        m_zoomAnim->stop();
        update();
        return;
    }
    m_zoomOldLine = oldLine;
    m_zoomNewLine = newLine;
    m_zoomAnim->stop();
    m_zoomAnim->setStartValue(0.0);
    m_zoomAnim->setEndValue(1.0);
    m_zoomAnim->start();
}

void LyricRenderer::paintEvent(QPaintEvent*)
{
    if (m_lines.isEmpty())
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(m_opacityPercent / 100.0); // Fresh painter: reset per paint.

    if (m_vertical)
        paintVertical(painter);
    else
        paintHorizontal(painter);
}

QFont LyricRenderer::makeMainFont(int lineIndex) const
{
    QFont font;
    if (!m_fontFamily.isEmpty())
        font.setFamily(m_fontFamily);
    // Active-line zoom: each line carries its own 0..1 progress (1.0 = fully
    // zoomed), so the incoming line grows while the outgoing line shrinks over
    // the same transition. 1.0x at progress 0, kActiveMainZoom at progress 1
    // (reference .lrcActiveZoom .line { font-size: 1.2em }).
    const qreal progress = (lineIndex >= 0 && lineIndex < m_lineZoomProgress.size())
        ? m_lineZoomProgress.at(lineIndex)
        : 0.0;
    const qreal zoom = m_zoomActiveLrc
        ? 1.0 + (kActiveMainZoom - 1.0) * progress
        : 1.0;
    font.setPixelSize(qRound(m_fontSize * zoom));
    font.setBold(m_fontWeightLine); // Every line renders line-mode style.
    return font;
}

QFont LyricRenderer::makeExtendedFont(int lineIndex) const
{
    QFont font;
    if (!m_fontFamily.isEmpty())
        font.setFamily(m_fontFamily);
    // Extended lines sit at kExtendedScale; the active zoomed group grows them
    // to kActiveExtendedZoom (reference .extended 0.8em -> .lrcActiveZoom
    // .active .extended .94em). Same per-line progress as the main line.
    const qreal progress = (lineIndex >= 0 && lineIndex < m_lineZoomProgress.size())
        ? m_lineZoomProgress.at(lineIndex)
        : 0.0;
    const qreal zoom = m_zoomActiveLrc
        ? kExtendedScale + (kActiveExtendedZoom - kExtendedScale) * progress
        : kExtendedScale;
    font.setPixelSize(qRound(m_fontSize * zoom));
    font.setBold(m_fontWeightExtended);
    return font;
}

int LyricRenderer::textFlags() const
{
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

QString LyricRenderer::elideForWidth(const QString& text, const QFontMetrics& fm, int availableWidth) const
{
    if (!m_ellipsis || text.isEmpty() || availableWidth <= 0)
        return text;
    if (fm.horizontalAdvance(text) <= availableWidth)
        return text;
    return fm.elidedText(text, Qt::ElideRight, availableWidth);
}

QString LyricRenderer::elideForHeight(const QString& text, const QFontMetrics& fm, int availableHeight, int letterSpacing) const
{
    if (!m_ellipsis || text.isEmpty() || availableHeight <= 0)
        return text;
    if (columnHeight(text, fm, letterSpacing) <= availableHeight)
        return text;
    // Qt 6.11 removed Qt::ElideBottom, so the vertical column is elided by
    // hand with the same semantics: drop trailing characters until the column
    // (ellipsis included) fits the available height, then append "…".
    const int step = fm.height() + letterSpacing;
    const int maxChars = availableHeight / step;
    if (maxChars <= 1)
        return QStringLiteral("…");
    return text.left(maxChars - 1) + QStringLiteral("…");
}

int LyricRenderer::columnWidth(const QString& text, const QFontMetrics& fm) const
{
    int widest = 0;
    for (const QChar ch : text)
        widest = qMax(widest, fm.horizontalAdvance(ch));
    return widest;
}

int LyricRenderer::columnHeight(const QString& text, const QFontMetrics& fm, int letterSpacing) const
{
    if (text.isEmpty())
        return 0;
    const int step = fm.height() + letterSpacing;
    return text.size() * step - letterSpacing; // N boxes, N-1 inter-char gaps.
}

LyricRenderer::VerticalGroupMetrics LyricRenderer::measureVerticalGroup(const RenderLine& line, int lineIndex) const
{
    VerticalGroupMetrics m;
    m.mainFont = makeMainFont(lineIndex);
    m.extendedFont = makeExtendedFont(lineIndex);
    const QFontMetrics mainFm(m.mainFont);
    const QFontMetrics extendedFm(m.extendedFont);

    const int availableHeight = height();
    // Every line renders line-mode style, so the vertical line-mode
    // letter-spacing applies to all columns.
    const int letterSpacing = kVerticalLetterSpacing;

    m.mainText = elideForHeight(stripWordTags(line.text), mainFm, availableHeight, letterSpacing);
    m.mainColumnWidth = columnWidth(m.mainText, mainFm);
    m.mainColumnHeight = columnHeight(m.mainText, mainFm, letterSpacing);
    m.groupWidth = m.mainColumnWidth;
    m.groupHeight = m.mainColumnHeight;

    for (const QString& rawExtended : line.extended) {
        const QString extText = elideForHeight(stripWordTags(rawExtended), extendedFm, availableHeight, letterSpacing);
        m.extendedTexts << extText;
        m.groupWidth += columnWidth(extText, extendedFm);
        m.groupHeight = qMax(m.groupHeight, columnHeight(extText, extendedFm, letterSpacing));
    }
    if (!line.extended.isEmpty())
        m.groupWidth += qRound(verticalExtendedGap(m_lineGap)) * line.extended.size();

    return m;
}

void LyricRenderer::paintHorizontal(QPainter& p)
{
    const qreal gap = m_lineGap;
    const qreal lineWidth = width();

    // Measure each group (main + extended rows) so the block can be centered,
    // then shift the whole block by the scroll offset (content moves up as the
    // offset grows).
    const HorizontalLayout layout = measureHorizontal();
    // The block starts below the 80% leading spacer (reference .lyricSpace), so
    // the active line can always center — even the first/last lines.
    const qreal yOffset = qRound(kLyricSpaceRatio * height());

    qreal y = yOffset - m_scrollOffset;
    for (int i = 0; i < m_lines.size(); ++i) {
        drawHorizontalGroup(p, m_lines.at(i), i, QRectF(0, y, lineWidth, layout.groupHeights.at(i)));
        y += layout.groupHeights.at(i) + gap;
    }
}

void LyricRenderer::paintVertical(QPainter& p)
{
    const qreal gap = verticalGroupGap(m_lineGap);

    // Measure every column group so the whole block can be centered; scrolling
    // shifts the block left (content moves left as the offset grows).
    const QVector<VerticalGroupMetrics> metrics = measureAllVerticalGroups();
    const qreal blockWidth = verticalBlockWidth(metrics);

    // The block starts after the 80% leading spacer (reference .lyricSpace
    // width:80%), so the active column can always center — even the first/last.
    const qreal xOffset = qRound(kLyricSpaceRatio * width());

    // writing-mode: vertical-rl — blocks stack right-to-left, line 0 rightmost.
    qreal right = xOffset + blockWidth - m_scrollOffset;
    for (int i = 0; i < m_lines.size(); ++i) {
        const qreal groupWidth = metrics.at(i).groupWidth;
        drawVerticalGroup(p, metrics.at(i), i,
                          QRectF(right - groupWidth, 0, groupWidth, metrics.at(i).groupHeight));
        right -= groupWidth + gap;
    }
}

void LyricRenderer::drawHorizontalGroup(QPainter& p, const RenderLine& line, int lineIndex, const QRectF& rect)
{
    const QFont mainFont = makeMainFont(lineIndex);
    const QFont extendedFont = makeExtendedFont(lineIndex);
    const QFontMetrics mainFm(mainFont);
    const QFontMetrics extendedFm(extendedFont);
    const qreal extGap = horizontalExtendedGap(m_lineGap);

    // Line-by-line highlight: the whole active line switches to the played
    // color, inactive lines stay unplay; the 600 ms transition interpolates
    // both (reference .line-mode.active .font-lrc; .extended .font-lrc shares
    // the stroke).
    const QColor fill = colorForLine(lineIndex);

    if (m_ellipsis) {
        // Ellipsis mode: one single line with a trailing "…" (reference
        // .ellipsis .font-lrc { .mixin-ellipsis(1) }).
        const QString mainText = elideForWidth(stripWordTags(line.text), mainFm, int(rect.width()));
        const QRectF mainRect(rect.x(), rect.y(), rect.width(), mainFm.height());
        drawTextWithStroke(p, mainText, mainRect, mainFont, fill, m_shadowColor, kLineStrokeWidth);

        qreal y = rect.y() + mainFm.height();
        for (const QString& rawExtended : line.extended) {
            y += extGap;
            const QString extText = elideForWidth(stripWordTags(rawExtended), extendedFm, int(rect.width()));
            drawTextWithStroke(p, extText, QRectF(rect.x(), y, rect.width(), extendedFm.height()),
                               extendedFont, fill, m_shadowColor, kLineStrokeWidth);
            y += extendedFm.height();
        }
        return;
    }

    // Wrap mode (reference .line-content overflow-wrap:break-word — there is
    // no separate wrap toggle): every row paints at one line-height. A wrapped
    // block taller than the rect is still fully drawn; the painter clips,
    // matching the reference where the layout height simply grows.
    qreal y = rect.y();
    const QStringList mainRows = wrapForWidth(stripWordTags(line.text), mainFm, int(rect.width()));
    for (const QString& row : mainRows) {
        drawTextWithStroke(p, row, QRectF(rect.x(), y, rect.width(), mainFm.height()),
                           mainFont, fill, m_shadowColor, kLineStrokeWidth);
        y += mainFm.height();
    }
    for (const QString& rawExtended : line.extended) {
        y += extGap;
        const QStringList extRows = wrapForWidth(stripWordTags(rawExtended), extendedFm, int(rect.width()));
        for (const QString& row : extRows) {
            drawTextWithStroke(p, row, QRectF(rect.x(), y, rect.width(), extendedFm.height()),
                               extendedFont, fill, m_shadowColor, kLineStrokeWidth);
            y += extendedFm.height();
        }
    }
}

void LyricRenderer::drawVerticalGroup(QPainter& p, const VerticalGroupMetrics& m, int lineIndex, const QRectF& rect)
{
    const QFontMetrics extendedFm(m.extendedFont);
    const int letterSpacing = kVerticalLetterSpacing; // Every line renders line-mode style.
    const qreal extGap = verticalExtendedGap(m_lineGap);

    // Line-by-line highlight: the whole active column group switches to the
    // played color, inactive groups stay unplay (color transition included).
    const QColor fill = colorForLine(lineIndex);

    // Main column sits at the right edge; extended columns go left of it, each
    // separated by the extended gap (reference .extended { margin-right: ... }).
    qreal right = rect.right();
    const QRectF mainRect(right - m.mainColumnWidth, rect.top(), m.mainColumnWidth, m.mainColumnHeight);
    drawVerticalText(p, m.mainText, mainRect, m.mainFont, fill, m_shadowColor, kLineStrokeWidth, letterSpacing);
    right = mainRect.left() - extGap;

    for (const QString& extText : m.extendedTexts) {
        const int colWidth = columnWidth(extText, extendedFm);
        const QRectF extRect(right - colWidth, rect.top(), colWidth, m.groupHeight);
        drawVerticalText(p, extText, extRect, m.extendedFont, fill, m_shadowColor, kLineStrokeWidth, letterSpacing);
        right = extRect.left() - extGap;
    }
}

void LyricRenderer::drawVerticalText(QPainter& p, const QString& text, const QRectF& columnRect, const QFont& font, const QColor& fill, const QColor& stroke, int strokeWidth, int letterSpacing)
{
    if (text.isEmpty())
        return;
    const QFontMetrics fm(font);
    const qreal step = fm.height() + letterSpacing;
    qreal y = columnRect.y();
    for (const QChar ch : text) {
        drawTextWithStroke(p, QString(ch), QRectF(columnRect.x(), y, columnRect.width(), fm.height()),
                           font, fill, stroke, strokeWidth);
        y += step;
    }
}

void LyricRenderer::drawTextWithStroke(QPainter& p, const QString& text, const QRectF& rect, const QFont& font, const QColor& fill, const QColor& stroke, int strokeWidth)
{
    if (text.isEmpty())
        return;

    p.setFont(font);
    const int flags = textFlags();

    // Four offset passes in the stroke color, then the fill on top
    // (reference .stroke3/.stroke4 for line-mode and extended lines).
    p.setPen(stroke);
    p.setBrush(stroke);
    p.drawText(rect.translated(strokeWidth, 0), flags, text);
    p.drawText(rect.translated(-strokeWidth, 0), flags, text);
    p.drawText(rect.translated(0, strokeWidth), flags, text);
    p.drawText(rect.translated(0, -strokeWidth), flags, text);

    p.setPen(fill);
    p.setBrush(fill);
    p.drawText(rect, flags, text);
}

LyricRenderer::HorizontalLayout LyricRenderer::measureHorizontal() const
{
    HorizontalLayout layout;
    layout.groupHeights.resize(m_lines.size());
    // The same width the paint pass draws into (the widget's own width), so a
    // wrapped line measures exactly as tall as it paints.
    const int wrapWidth = width();
    for (int i = 0; i < m_lines.size(); ++i) {
        const RenderLine& line = m_lines.at(i);
        const QFontMetrics mainFm(makeMainFont(i));
        const QFontMetrics extendedFm(makeExtendedFont(i));
        qreal groupHeight = 0;
        if (m_ellipsis) {
            groupHeight += mainFm.height();
            if (!line.extended.isEmpty())
                groupHeight += line.extended.size() * (horizontalExtendedGap(m_lineGap) + extendedFm.height());
        } else {
            groupHeight += wrapForWidth(stripWordTags(line.text), mainFm, wrapWidth).size() * mainFm.height();
            for (const QString& rawExtended : line.extended)
                groupHeight += horizontalExtendedGap(m_lineGap)
                    + wrapForWidth(stripWordTags(rawExtended), extendedFm, wrapWidth).size() * extendedFm.height();
        }
        layout.groupHeights[i] = groupHeight;
        layout.blockHeight += groupHeight;
    }
    if (m_lines.size() > 1)
        layout.blockHeight += m_lineGap * (m_lines.size() - 1);
    return layout;
}

QVector<LyricRenderer::VerticalGroupMetrics> LyricRenderer::measureAllVerticalGroups() const
{
    QVector<VerticalGroupMetrics> metrics(m_lines.size());
    for (int i = 0; i < m_lines.size(); ++i)
        metrics[i] = measureVerticalGroup(m_lines.at(i), i);
    return metrics;
}

qreal LyricRenderer::verticalBlockWidth(const QVector<VerticalGroupMetrics>& metrics) const
{
    qreal blockWidth = 0;
    for (const VerticalGroupMetrics& m : metrics)
        blockWidth += m.groupWidth;
    if (metrics.size() > 1)
        blockWidth += verticalGroupGap(m_lineGap) * (metrics.size() - 1);
    return blockWidth;
}

qreal LyricRenderer::maxScrollOffset() const
{
    if (m_lines.isEmpty())
        return 0;
    const qreal blockExtent = m_vertical
        ? verticalBlockWidth(measureAllVerticalGroups())
        : measureHorizontal().blockHeight;
    const int viewportExtent = m_vertical ? width() : height();
    // The block sits below an 80% leading spacer with an 80% trailing spacer
    // (reference .lyricSpace), so the scrollable extent is 1.6 * viewport +
    // block; the usable range holds kLyricSpaceInnerFactor of viewport
    // headroom beyond the block — always > 0 while lines exist.
    return qMax<qreal>(0, kLyricSpaceInnerFactor * viewportExtent + blockExtent);
}

qreal LyricRenderer::autoScrollTarget() const
{
    if (m_lines.isEmpty() || m_activeLine < 0 || m_activeLine >= m_lines.size())
        return 0;

    if (m_vertical) {
        const QVector<VerticalGroupMetrics> metrics = measureAllVerticalGroups();
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
        const qreal target = m_scrollAlignTop ? (width() - groupExtent - 2) : (width() - groupExtent) / 2.0;
        return qBound<qreal>(0, baseLeft - target, maxScrollOffset());
    }

    const HorizontalLayout layout = measureHorizontal();
    // Leading 80% spacer: the block's top edge starts here, so even the last
    // line can center (reference .lyricSpace height:80%).
    const qreal yOffset = qRound(kLyricSpaceRatio * height());
    qreal baseTop = yOffset;
    for (int i = 0; i < m_activeLine; ++i)
        baseTop += layout.groupHeights.at(i) + m_lineGap;

    const qreal groupExtent = layout.groupHeights.at(m_activeLine);
    const qreal target = m_scrollAlignTop ? 0 : (height() - groupExtent) / 2.0;
    return qBound<qreal>(0, baseTop - target, maxScrollOffset());
}

void LyricRenderer::scrollToActiveInstant()
{
    m_delayScrollTimer->stop();
    if (m_scrollAnimation)
        m_scrollAnimation->stop();
    m_scrollOffset = autoScrollTarget();
    update();
}

void LyricRenderer::scrollToActiveAnimated(int durationMs)
{
    m_delayScrollTimer->stop();
    if (m_scrollAnimation)
        m_scrollAnimation->stop();

    const qreal target = autoScrollTarget();
    if (durationMs <= 0 || qAbs(target - m_scrollOffset) < 0.5) {
        m_scrollOffset = target;
        update();
        return;
    }
    m_scrollAnimation->setStartValue(m_scrollOffset);
    m_scrollAnimation->setEndValue(target);
    m_scrollAnimation->setDuration(durationMs);
    m_scrollAnimation->start();
}

void LyricRenderer::suspendAutoScroll()
{
    m_userScrolling = true;
    if (m_scrollAnimation)
        m_scrollAnimation->stop();
    m_delayScrollTimer->stop();
}

void LyricRenderer::rearmResumeTimer()
{
    m_resumeTimer->start();
}

void LyricRenderer::startDelayScrollTimer()
{
    m_delayScrollTimer->start();
}

void LyricRenderer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_lines.isEmpty())
        return;
    // The block re-centers on resize; clamp any manual offset to the new range
    // and, when not mid-interaction, keep the active line at its target.
    m_scrollOffset = qBound<qreal>(0, m_scrollOffset, maxScrollOffset());
    if (!m_userScrolling)
        scrollToActiveInstant();
    update();
}

void LyricRenderer::wheelEvent(QWheelEvent* event)
{
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
        m_scrollOffset = qBound<qreal>(0, m_scrollOffset + delta, maxScrollOffset());
        update();
        rearmResumeTimer();
    }
    event->accept();
}

void LyricRenderer::mousePressEvent(QMouseEvent* event)
{
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

void LyricRenderer::mouseMoveEvent(QMouseEvent* event)
{
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

void LyricRenderer::mouseReleaseEvent(QMouseEvent* event)
{
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
