/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
// Regression tests for the LyricRenderer transition and scroll engines:
// fast line changes must not snap outgoing lines back to the full played
// color / full zoom (CSS continue-from-current semantics), the scroll must
// keep advancing while line changes outpace the animation (reference
// handleScrollY queue), and a burst of consecutive lines with the delayed
// scroll must roll instead of freezing and then jumping.
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTest>
#include <QVector>

#include "renderer/lyricrenderer.h"
#include "testbootstrap.h"

namespace {

QVector<RenderLine> makeLines(int count)
{
  QVector<RenderLine> lines;
  lines.reserve(count);
  for (int i = 0; i < count; ++i)
    lines.append(RenderLine{QStringLiteral("Line number %1 with some words").arg(i + 1), {}});
  return lines;
}

} // namespace

class TestLyricRenderer : public QObject {
  Q_OBJECT
private slots:
  void fastChangesDoNotSnapColorBack();
  void fastChangesDoNotSnapZoomBack();
  void scrollKeepsUpWithRapidChanges();
  void delayScrollBurstConverges();
  void delayedScrollRetargetsToCurrentLine();
  void negativeLineDoesNotScroll();
  void rePushSettlesAtCenteredLineZero();
  void setChangeCentersFirstLine();
  void strokeOffsetsMatchReference();
  void outlineVisibleOnWhiteBackground();
  void zoomGrowsSmoothlyBetweenProgressSteps();
  void zoomStaysAnchoredToAlignment();
  void lineFlushToAlignedBorder();
  void lineBlitOpacityFollowsBodyOpacityAndColorProgress();
};

void TestLyricRenderer::fastChangesDoNotSnapColorBack()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(4));
  renderer.setActiveLine(0);
  QTest::qWait(700); // Settle: line 0 fully played.
  QVERIFY(qAbs(renderer.colorProgressForLine(0) - 1.0) < 0.01);

  renderer.setActiveLine(1);
  // Line 1 mid-grow: OutCubic over 600 ms gives ~0.58 at exactly 150 ms,
  // but the progress is sampled at the last 16 ms transition tick, so the
  // observed value jitters with timer scheduling (~0.53-0.68). The bound
  // only needs to prove the transition is underway and not instant.
  QTest::qWait(150);
  const double mid = renderer.colorProgressForLine(1);
  QVERIFY2(mid > 0.1 && mid < 0.75, qPrintable(QStringLiteral("mid=%1").arg(mid)));

  // A second change inside the transition: the outgoing line continues from
  // its CURRENT progress — the old shared cross-fade snapped it back to the
  // full played color (1.0) here, leaving a green trace during fast lyrics.
  renderer.setActiveLine(2);
  QTest::qWait(100);
  const double outgoing = renderer.colorProgressForLine(1);
  QVERIFY2(outgoing < 0.75, qPrintable(QStringLiteral("outgoing=%1").arg(outgoing)));
  const double incoming = renderer.colorProgressForLine(2);
  QVERIFY2(incoming > 0.1 && incoming < 0.7,
           qPrintable(QStringLiteral("incoming=%1").arg(incoming)));

  // Everything settles: outgoing at unplay, incoming at played.
  QTest::qWait(900);
  QVERIFY(qAbs(renderer.colorProgressForLine(1)) < 0.01);
  QVERIFY(qAbs(renderer.colorProgressForLine(2) - 1.0) < 0.01);
  QVERIFY(qAbs(renderer.colorProgressForLine(0)) < 0.01);
}

void TestLyricRenderer::fastChangesDoNotSnapZoomBack()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(4));
  renderer.setActiveLine(0);
  QTest::qWait(700); // Settle: line 0 fully zoomed.
  QVERIFY(qAbs(renderer.zoomProgress() - 1.0) < 0.01);

  renderer.setActiveLine(1);
  // Same sampling jitter as the color test: the value at 150 ms is ~0.53-0.68.
  QTest::qWait(150);
  const double mid = renderer.zoomProgress(); // Active line 1 mid-grow.
  QVERIFY2(mid > 0.1 && mid < 0.75, qPrintable(QStringLiteral("mid=%1").arg(mid)));

  // Outgoing line 1 must decay from its current zoom — not snap back to 1.0.
  renderer.setActiveLine(2);
  QTest::qWait(100);
  QVERIFY2(renderer.zoomProgressForLine(1) < 0.75,
           qPrintable(QStringLiteral("outgoing zoom=%1").arg(renderer.zoomProgressForLine(1))));

  QTest::qWait(900);
  QVERIFY(qAbs(renderer.zoomProgressForLine(1)) < 0.01);
  QVERIFY(qAbs(renderer.zoomProgress() - 1.0) < 0.01);
}

void TestLyricRenderer::scrollKeepsUpWithRapidChanges()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(12));
  renderer.setDelayScroll(false);
  renderer.setActiveLine(0);
  QTest::qWait(700); // The first line-0 scrolls to its centered target (reference initLrc).
  const double baseline = renderer.scrollOffset();
  QVERIFY(baseline > 50);

  // 11 line changes at 50 ms intervals: each is a jump from the current
  // offset's perspective. The reference handleScrollY queue keeps the
  // animation advancing (restart at 75% of each run), so the view must be
  // most of the way to the final target right after the burst — the old
  // restart-from-zero easing crawled at ~5% of the remaining distance per
  // change and ended the burst ~6% of the way there.
  for (int i = 1; i < 12; ++i) {
    renderer.setActiveLine(i);
    QTest::qWait(50);
  }
  const double atBurstEnd = renderer.scrollOffset();

  QTest::qWait(500); // Let the final animation finish.
  const double settled = renderer.scrollOffset();
  QVERIFY2(settled > baseline + 40, qPrintable(QStringLiteral("settled=%1").arg(settled)));

  // The view must have been most of the way to the target at burst end,
  // not crawling behind the current line.
  QVERIFY2(atBurstEnd > baseline + 0.5 * (settled - baseline),
           qPrintable(QStringLiteral("atBurstEnd=%1 settled=%2 baseline=%3")
                        .arg(atBurstEnd)
                        .arg(settled)
                        .arg(baseline)));

  // Convergence: one more wait changes nothing.
  QTest::qWait(400);
  QVERIFY2(qAbs(renderer.scrollOffset() - settled) < 1.0,
           qPrintable(QStringLiteral("drift=%1").arg(renderer.scrollOffset() - settled)));
}

void TestLyricRenderer::delayScrollBurstConverges()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(8));
  renderer.setDelayScroll(true);
  renderer.setActiveLine(0);
  QTest::qWait(700); // The first line-0 scrolls to its centered target (reference initLrc).
  const double baseline = renderer.scrollOffset();
  QVERIFY(baseline > 50);

  // A burst of consecutive steps: the pending 600 ms roll is armed once and
  // retargets to the current line when it fires (the old per-change re-arm
  // froze the view for the whole burst and then rolled one long jump).
  for (int i = 1; i <= 5; ++i) {
    renderer.setActiveLine(i);
    QTest::qWait(100);
  }
  // The single armed roll fires ~600 ms after the burst's first change and
  // runs 600 ms: ~750 ms after the burst the view must already be most of
  // the way there (the old code had not started rolling until 600 ms after
  // the LAST change).
  QTest::qWait(750);
  const double atRollMid = renderer.scrollOffset();
  QTest::qWait(1200);
  const double settled = renderer.scrollOffset();
  QVERIFY2(settled > baseline + 40, qPrintable(QStringLiteral("settled=%1").arg(settled)));
  QVERIFY2(atRollMid > baseline + 0.7 * (settled - baseline),
           qPrintable(QStringLiteral("atRollMid=%1 settled=%2 baseline=%3")
                        .arg(atRollMid)
                        .arg(settled)
                        .arg(baseline)));
  QTest::qWait(800);
  QVERIFY2(qAbs(renderer.scrollOffset() - settled) < 1.0,
           qPrintable(QStringLiteral("drift=%1").arg(renderer.scrollOffset() - settled)));
}

void TestLyricRenderer::delayedScrollRetargetsToCurrentLine()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(8));
  renderer.setDelayScroll(true);
  renderer.setActiveLine(0);
  QTest::qWait(700); // The first line-0 scrolls to its centered target (reference initLrc).
  const double baseline = renderer.scrollOffset();
  QVERIFY(baseline > 50);

  // Consecutive steps at a comfortable pace: the roll must land on the
  // line that was current when it fired, i.e. line 3, not line 1.
  for (int i = 1; i <= 3; ++i) {
    renderer.setActiveLine(i);
    QTest::qWait(700);
  }
  QTest::qWait(1500); // Last roll: 600 ms delay + 600 ms animation.
  const double settled = renderer.scrollOffset();
  QVERIFY2(settled > baseline + 50, qPrintable(QStringLiteral("settled=%1").arg(settled)));

  // Line 3 is far beyond line 1: a roll that stopped at the first change
  // would sit far below the final position.
  QTest::qWait(700);
  QVERIFY(qAbs(renderer.scrollOffset() - settled) < 1.0);
}

void TestLyricRenderer::negativeLineDoesNotScroll()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(12));
  renderer.setDelayScroll(false);
  renderer.setActiveLine(1);
  QTest::qWait(700); // Settle at line 1.
  const double offset = renderer.scrollOffset();
  QVERIFY(offset > 50);

  // A negative active line (the reference lead-in / cleared state) must not
  // animate the view back to the top: reference scrollLine returns before
  // scrolling. Pre-fix setActiveLine(-1) animated to 0.
  renderer.setActiveLine(-1);
  QTest::qWait(400);
  QCOMPARE(renderer.scrollOffset(), offset);
}

void TestLyricRenderer::rePushSettlesAtCenteredLineZero()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(12));
  renderer.setDelayScroll(false);
  renderer.setActiveLine(6);
  QTest::qWait(700); // Settle at line 6.
  QVERIFY(renderer.scrollOffset() > 50);

  // Simulate the host re-push: a fresh setLines resets the scroll AND the
  // active line, so the first setActiveLine(0) scrolls to line 0's CENTERED
  // target (reference initLrc -> nextTick -> handleScrollLrc). Pre-fix
  // setLines snapped to line 6's offset and the swallowed line-0 left the
  // view parked at the top.
  renderer.setLines(makeLines(12));
  renderer.setActiveLine(0);
  const qreal justAfter = renderer.scrollOffset();
  QVERIFY2(justAfter < 50,
           qPrintable(QStringLiteral("justAfter=%1").arg(justAfter))); // No stale snap to line 6.

  QTest::qWait(400); // The 300 ms centering scroll finishes.
  const qreal settled = renderer.scrollOffset();
  QVERIFY2(settled > 50 && settled < 200,
           qPrintable(
             QStringLiteral("settled=%1").arg(settled))); // Centered line 0 — not top, not line 6.

  QTest::qWait(300);
  QVERIFY(qAbs(renderer.scrollOffset() - settled) < 1.0); // No roll back.
}

void TestLyricRenderer::setChangeCentersFirstLine()
{
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(12));
  renderer.setDelayScroll(false);
  renderer.setActiveLine(0);
  QTest::qWait(500); // The first line-0 scrolls to its centered target.
  const qreal lineZero = renderer.scrollOffset();
  QVERIFY2(lineZero > 50 && lineZero < 200,
           qPrintable(QStringLiteral("lineZero=%1").arg(lineZero)));

  // A real consecutive line change still scrolls normally on top of that.
  renderer.setActiveLine(1);
  QTest::qWait(700);
  QVERIFY(renderer.scrollOffset() > lineZero + 10);
}

void TestLyricRenderer::strokeOffsetsMatchReference()
{
  // Parity guard for the replicated stroke3/stroke4 text-shadow tables
  // (references/src/renderer-lyric/assets/styles/layout.less: .stroke3
  // lines 120-151, .stroke4 lines 52-76). Duplicates are part of the
  // reference — each entry is one alpha-composited pass — so the counts are
  // asserted too. Fuzzy compares avoid FP rounding brittleness.
  const auto containsOffset = [](const QVector<QPointF>& offsets, QPointF expected) {
    for (const QPointF& offset : offsets) {
      if (qAbs(offset.x() - expected.x()) < 1e-6 && qAbs(offset.y() - expected.y()) < 1e-6)
        return true;
    }
    return false;
  };

  // stroke3 at em=100: 32 em offsets -> 0.04em = 4 px, 0.01em = 1 px.
  const QVector<QPointF> stroke3 =
    LyricRenderer::strokeOffsetsPx(LyricRenderer::StrokeStyle::Stroke3, 100.0);
  QVERIFY(stroke3.size() == 32);
  const QVector<QPointF> stroke3Samples{
    QPointF(4, 4), QPointF(4, -3), QPointF(-4, -3), QPointF(-4, 4),
    QPointF(1, 0), QPointF(0, -1), QPointF(0, 4),   QPointF(0, 1),
  };
  for (const QPointF& sample : stroke3Samples)
    QVERIFY2(containsOffset(stroke3, sample),
             qPrintable(
               QStringLiteral("stroke3 missing offset (%1, %2)").arg(sample.x()).arg(sample.y())));

  // em scaling is linear: em=50 is exactly half of em=100.
  const QVector<QPointF> stroke3Half =
    LyricRenderer::strokeOffsetsPx(LyricRenderer::StrokeStyle::Stroke3, 50.0);
  QVERIFY(stroke3Half.size() == stroke3.size());
  for (int i = 0; i < stroke3.size(); ++i) {
    QVERIFY(qAbs(stroke3Half.at(i).x() - stroke3.at(i).x() / 2.0) < 1e-6);
    QVERIFY(qAbs(stroke3Half.at(i).y() - stroke3.at(i).y() / 2.0) < 1e-6);
  }

  // stroke4 at em=100: 17 mixed em/px offsets. 0.02em x 100 = 2 px; px
  // entries stay at their fixed device pixels.
  const QVector<QPointF> stroke4 =
    LyricRenderer::strokeOffsetsPx(LyricRenderer::StrokeStyle::Stroke4, 100.0);
  QVERIFY(stroke4.size() == 17);
  const QVector<QPointF> stroke4Samples{
    QPointF(2, -2),  // 0.02em x, -0.02em y
    QPointF(-2, -1), // -0.02em x, -1px y
    QPointF(-1, -1), // -1px, -1px
    QPointF(1, 0),   // 1px, 0px
  };
  for (const QPointF& sample : stroke4Samples)
    QVERIFY2(containsOffset(stroke4, sample),
             qPrintable(
               QStringLiteral("stroke4 missing offset (%1, %2)").arg(sample.x()).arg(sample.y())));

  // px stability: the px entries are em-independent even at em=1000.
  const QVector<QPointF> stroke4Huge =
    LyricRenderer::strokeOffsetsPx(LyricRenderer::StrokeStyle::Stroke4, 1000.0);
  QVERIFY(containsOffset(stroke4Huge, QPointF(-1, -1)));
  QVERIFY(containsOffset(stroke4Huge, QPointF(1, 0)));
}

void TestLyricRenderer::outlineVisibleOnWhiteBackground()
{
  // Acceptance proof: white text on a white image with no active line — the
  // fill stays white (unplay), so any pixel darker than the background must
  // come from the stroke3 halo (the reference .stroke3 stack drawn in the
  // 51%-alpha shadow shade).
  LyricRenderer renderer;
  renderer.resize(600, 200);
  // Dump helper: the acceptance size is 48 px, but LX_LYRICS_DUMP_FONT_SIZE
  // overrides it (e.g. 14 px, the app default) for a representative paint
  // dump. Unset/invalid keeps 48, so the default run renders byte-identically.
  bool sizeOk = false;
  int fontSize = qEnvironmentVariableIntValue("LX_LYRICS_DUMP_FONT_SIZE", &sizeOk);
  if (!sizeOk || fontSize <= 0)
    fontSize = 48;
  renderer.setFontSize(fontSize);
  renderer.setLines(QVector<RenderLine>{RenderLine{QStringLiteral("Test"), {}}});

  QImage image(renderer.size(), QImage::Format_ARGB32);
  image.fill(Qt::white);
  {
    QPainter painter(&image);
    renderer.render(&painter);
  }

  int dark = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (qGray(image.pixel(x, y)) < 170)
        ++dark;
    }
  }

  // The reference halo at 48 px leaves a dense ~gray-100 outline (the old
  // 1 px 4-pass outline peaked at ~gray 210+ and read as invisible on
  // white); it stays an outline, never a blob — bound it well below a tenth
  // of the image.
  QVERIFY2(dark > 20, qPrintable(QStringLiteral("dark=%1").arg(dark)));
  QVERIFY2(dark < image.width() * image.height() / 10,
           qPrintable(QStringLiteral("dark=%1").arg(dark)));

  const QString dumpPath = qEnvironmentVariable("LX_LYRICS_DUMP_PAINT");
  if (!dumpPath.isEmpty())
    image.save(dumpPath);
}

void TestLyricRenderer::zoomGrowsSmoothlyBetweenProgressSteps()
{
  // Acceptance proof for the smooth active-line zoom: the painted glyph must
  // grow CONTINUOUSLY with the zoom progress — the old qRound(m_fontSize *
  // zoom) font held the size flat across 2-3 transition steps (Qt rounds
  // font sizes to whole pixels) and then jumped ~2 px. The glyph extent is
  // measured at sub-pixel precision via its coverage profile, so the ~0.4
  // px growth per 0.02 progress step is detectable.
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setFontSize(48);
  renderer.setLines(QVector<RenderLine>{RenderLine{QStringLiteral("Test"), {}}});
  renderer.setActiveLine(0);
  QTest::qWait(700); // Settle: line 0 fully played AND fully zoomed.

  const QString dumpPath = qEnvironmentVariable("LX_LYRICS_DUMP_PAINT");
  const auto paintedExtent = [&](double progress) {
    renderer.setZoomProgressForLine(0, progress);
    QImage image(renderer.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    {
      QPainter painter(&image);
      renderer.render(&painter);
    }
    if (!dumpPath.isEmpty())
      image.save(dumpPath + QStringLiteral("_p%1.png").arg(int(progress * 100)));
    // Greenish coverage per column: t = (G-R)/190 is the linear
    // fill-coverage factor toward the played green (190 = G-R of the pure
    // played color), so antialiased edge pixels count fractionally; the
    // gray shadow strokes have G-R ~ 0 and drop out. Summing the per-column
    // max over the line is the glyph's silhouette extent — a continuous,
    // monotonic-in-scale quantity (each column's coverage only grows as
    // the painter-scale zoom magnifies the glyph).
    double extent = 0;
    for (int x = 0; x < image.width(); ++x) {
      double columnCoverage = 0;
      for (int y = 0; y < image.height(); ++y) {
        const QColor px = image.pixelColor(x, y);
        const double t = qBound(0.0, (px.green() - px.red()) / 190.0, 1.0);
        columnCoverage = qMax(columnCoverage, t);
      }
      extent += columnCoverage;
    }
    return extent;
  };

  double prev = paintedExtent(0.40);
  for (int step = 1; step <= 10; ++step) {
    const double p = 0.40 + 0.02 * step; // 0.42 .. 0.60 (scale 1.08..1.12)
    const double cur = paintedExtent(p);
    const double delta = cur - prev;
    // Every 0.02 progress step must grow the glyph by ~0.4 px. The old
    // rounded-font zoom produced delta 0 (plateau) or >= 1.6 px (jump) —
    // either failure mode trips this bound.
    QVERIFY2(delta > 0.05 && delta < 1.2,
             qPrintable(QStringLiteral("p=%1 delta=%2").arg(p).arg(delta)));
    prev = cur;
  }
}

void TestLyricRenderer::zoomStaysAnchoredToAlignment()
{
  // The zoom must grow from the ALIGNED edge, not from the text center:
  // right-aligned lyrics growing from a center pivot push past the widget's
  // right edge (reported bug: "it grows out of bound"). Mirroring CSS
  // text-align reflow, the anchored edge stays put while the free edge
  // extends away from it — right grows leftward, left grows rightward.
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setFontSize(48);
  renderer.setLines(QVector<RenderLine>{RenderLine{QStringLiteral("Test"), {}}});
  renderer.setActiveLine(0);
  QTest::qWait(700); // Settle: line 0 fully played AND fully zoomed.

  const QString dumpPath = qEnvironmentVariable("LX_LYRICS_DUMP_PAINT");
  // Edge columns of the greenish silhouette (same coverage metric as
  // zoomGrowsSmoothlyBetweenProgressSteps; >0.5 = solid glyph): returns
  // {leftmost, rightmost} painted column of the played-green fill.
  const auto paintedEdges = [&](double progress) {
    renderer.setZoomProgressForLine(0, progress);
    QImage image(renderer.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    {
      QPainter painter(&image);
      renderer.render(&painter);
    }
    if (!dumpPath.isEmpty())
      image.save(dumpPath + QStringLiteral("_a%1.png").arg(int(progress * 100)));
    int left = -1;
    int right = -1;
    for (int x = 0; x < image.width(); ++x) {
      double columnCoverage = 0;
      for (int y = 0; y < image.height(); ++y) {
        const QColor px = image.pixelColor(x, y);
        columnCoverage = qMax(columnCoverage, qBound(0.0, (px.green() - px.red()) / 190.0, 1.0));
      }
      if (columnCoverage > 0.5) {
        if (left < 0)
          left = x;
        right = x;
      }
    }
    return QPair<int, int>(left, right);
  };

  // Right-aligned: the right edge stays fixed while the text grows left.
  renderer.setAlign(Qt::AlignRight);
  const QPair<int, int> r0 = paintedEdges(0.0);
  const QPair<int, int> r1 = paintedEdges(1.0);
  QVERIFY2(r0.first >= 0, "no text painted (right-aligned)");
  QVERIFY2(qAbs(r1.second - r0.second) <= 1,
           qPrintable(QStringLiteral("right edge moved: %1 -> %2").arg(r0.second).arg(r1.second)));
  QVERIFY2(r1.first <= r0.first - 8,
           qPrintable(
             QStringLiteral("left edge did not grow left: %1 -> %2").arg(r0.first).arg(r1.first)));

  // Left-aligned: the left edge stays fixed while the text grows right.
  renderer.setAlign(Qt::AlignLeft);
  const QPair<int, int> l0 = paintedEdges(0.0);
  const QPair<int, int> l1 = paintedEdges(1.0);
  QVERIFY2(l0.first >= 0, "no text painted (left-aligned)");
  QVERIFY2(qAbs(l1.first - l0.first) <= 1,
           qPrintable(QStringLiteral("left edge moved: %1 -> %2").arg(l0.first).arg(l1.first)));
  QVERIFY2(
    l1.second >= l0.second + 8,
    qPrintable(
      QStringLiteral("right edge did not grow right: %1 -> %2").arg(l0.second).arg(l1.second)));
}

void TestLyricRenderer::lineFlushToAlignedBorder()
{
  // Regression for the cache halo geometry: the group was drawn at a
  // NEGATIVE offset inside its pixmap, clipping glyph tops and leaving
  // right-aligned lyrics floating ~16 px short of the right border
  // (reported: "gap between the lyrics and the border"; "upper part of each
  // line is not displayed"). The painted group must sit exactly on the
  // aligned edge.
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setFontSize(48);
  renderer.setLines(QVector<RenderLine>{RenderLine{QStringLiteral("Test"), {}}});
  renderer.setActiveLine(0);
  QTest::qWait(700); // Settle: line 0 fully played AND fully zoomed.

  const auto paintedEdges = [&](double progress) {
    renderer.setZoomProgressForLine(0, progress);
    QImage image(renderer.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    {
      QPainter painter(&image);
      renderer.render(&painter);
    }
    int left = -1;
    int right = -1;
    for (int x = 0; x < image.width(); ++x) {
      double columnCoverage = 0;
      for (int y = 0; y < image.height(); ++y) {
        const QColor px = image.pixelColor(x, y);
        columnCoverage = qMax(columnCoverage, qBound(0.0, (px.green() - px.red()) / 190.0, 1.0));
      }
      if (columnCoverage > 0.5) {
        if (left < 0)
          left = x;
        right = x;
      }
    }
    return QPair<int, int>(left, right);
  };

  // Right-aligned: the last painted column must sit at the right border (the
  // bug left it ~16 px short); left-aligned: flush at the left border.
  renderer.setAlign(Qt::AlignRight);
  const QPair<int, int> r = paintedEdges(1.0);
  QVERIFY2(r.first >= 0, "no text painted (right-aligned)");
  QVERIFY2(
    r.second >= renderer.width() - 3,
    qPrintable(
      QStringLiteral("right edge %1 not flush to border %2").arg(r.second).arg(renderer.width())));

  renderer.setAlign(Qt::AlignLeft);
  const QPair<int, int> l = paintedEdges(1.0);
  QVERIFY2(l.first >= 0, "no text painted (left-aligned)");
  QVERIFY2(l.first <= 3, qPrintable(QStringLiteral("left edge %1 not flush").arg(l.first)));
}

void TestLyricRenderer::lineBlitOpacityFollowsBodyOpacityAndColorProgress()
{
  // The blit opacity is the configured window opacity times the reference
  // body dimming lerped by the line's color progress: non-active lines stay
  // dimmed (0.8 * opacity), the active line reaches the FULL configured
  // color (1.0 * opacity), and mid-transition lines interpolate — the
  // QGraphicsOpacityEffect cannot exceed 1.0, so the dimming must live in
  // the per-line blit for the active line to ever reach its color.
  LyricRenderer renderer;
  renderer.resize(600, 300);
  renderer.setLines(makeLines(1)); // setColorProgressForLine needs a line.

  // Default body opacity (1.0): the renderer alone paints at full strength.
  QCOMPARE(renderer.lineBlitOpacity(0), 1.0);

  renderer.setBodyOpacity(0.8); // LyricWindow::kBodyOpacity.
  // Non-active line: the reference body dimming only.
  QCOMPARE(renderer.lineBlitOpacity(0), 0.8);
  // Mid-transition: the lerp keeps the 600 ms color fade continuous.
  renderer.setColorProgressForLine(0, 0.5);
  QCOMPARE(renderer.lineBlitOpacity(0), 0.9);
  // Active line: the full configured color, unclamped by the effect.
  renderer.setColorProgressForLine(0, 1.0);
  QCOMPARE(renderer.lineBlitOpacity(0), 1.0);

  // The configured window opacity still compounds on top.
  renderer.setOpacityPercent(50);
  QCOMPARE(renderer.lineBlitOpacity(0), 0.5);

  // Out-of-range lines are safe before/without any lines (colorProgress 0).
  LyricRenderer emptyRenderer;
  QCOMPARE(emptyRenderer.lineBlitOpacity(0), 1.0);
}

QTEST_MAIN(TestLyricRenderer)
#include "tst_renderer.moc"
