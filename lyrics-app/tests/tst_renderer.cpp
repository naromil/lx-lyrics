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
#include <QTest>
#include <QVector>

#include "renderer/lyricrenderer.h"

// Run headless: the renderer needs no real window system, and the suite must
// work in CI/ctest without a display. (Static init runs before QApplication.)
static const bool s_forceOffscreen = [] {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}();

namespace {

QVector<RenderLine> makeLines(int count)
{
    QVector<RenderLine> lines;
    lines.reserve(count);
    for (int i = 0; i < count; ++i)
        lines.append(RenderLine{ QStringLiteral("Line number %1 with some words").arg(i + 1), {} });
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
    QTest::qWait(150); // Line 1 mid-grow (OutCubic 600 ms -> ~0.35).
    const double mid = renderer.colorProgressForLine(1);
    QVERIFY2(mid > 0.1 && mid < 0.6, qPrintable(QStringLiteral("mid=%1").arg(mid)));

    // A second change inside the transition: the outgoing line continues from
    // its CURRENT progress — the old shared cross-fade snapped it back to the
    // full played color (1.0) here, leaving a green trace during fast lyrics.
    renderer.setActiveLine(2);
    QTest::qWait(100);
    const double outgoing = renderer.colorProgressForLine(1);
    QVERIFY2(outgoing < 0.6, qPrintable(QStringLiteral("outgoing=%1").arg(outgoing)));
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
    QTest::qWait(150);
    const double mid = renderer.zoomProgress(); // Active line 1 mid-grow.
    QVERIFY2(mid > 0.1 && mid < 0.6, qPrintable(QStringLiteral("mid=%1").arg(mid)));

    // Outgoing line 1 must decay from its current zoom — not snap back to 1.0.
    renderer.setActiveLine(2);
    QTest::qWait(100);
    QVERIFY2(renderer.zoomProgressForLine(1) < 0.6,
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
    QTest::qWait(700); // Settle at the first line (the leading spacer centers it).
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
                            .arg(atBurstEnd).arg(settled).arg(baseline)));

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
    QTest::qWait(700); // Settle at the first line (the leading spacer centers it).
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
                            .arg(atRollMid).arg(settled).arg(baseline)));
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
    QTest::qWait(700);
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

QTEST_MAIN(TestLyricRenderer)
#include "tst_renderer.moc"
