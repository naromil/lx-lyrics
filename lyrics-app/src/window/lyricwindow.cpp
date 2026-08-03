/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "window/lyricwindow.h"

#include <QCursor>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"
#include "renderer/controlbar.h"
#include "renderer/spectrumwidget.h"
#include "settings/settingsdialog.h"

namespace {

// Reference App.vue look: #main has border-radius 4px and a static
// rgba(0,0,0,.2) background with NO border; body { opacity: .8 } dims the
// whole window underneath the container fade.
constexpr int kRadiusBorder = 4;
constexpr int kAlwaysOnTopReassertMs = 500;

constexpr int kResizeHandleSize = 8; // 8 px edge/corner hit regions.

// Task E: after a compositor-native move/resize (startSystemMove /
// startSystemResize) the platform stops delivering move/resize events when the
// user releases; this idle window turns that quiet stream into one final
// saveBounds() + flush.
constexpr int kNativeOpSaveDebounceMs = 200;

// Pause-faint (Task D): the container fades between 1.0 and 0.05 over 300 ms
// (reference #container transition: opacity .3s ease, .hide { opacity: .05 }).
constexpr int kFadeAnimationMs = 300;
constexpr double kFaintFactor = 0.05;
constexpr double kBodyOpacity = 0.8; // reference body { opacity: .8 }

// Pane-fill fade (parity item 1): the pane background cross-fades between
// rgba(0,0,0,.2) and fully transparent over 400 ms when the window locks
// (reference #main transition: background-color @transition-theme, where
// @transition-theme = .4s ease in variables.less:19).
constexpr int kPaneAnimationMs = 400;

// Hover-hide (Task H.1): the reference mouseCheckTools polls the cursor every
// 500 ms while the window is locked and isHoverHide is on (setTimeout chain in
// mouseCheckTools.ts), fading the content to kFaintFactor while the cursor is
// over the window and restoring it once the cursor leaves.
constexpr int kHoverPollMs = 500;

const QColor kPaneFill(0, 0, 0, 51); // rgba(0, 0, 0, 0.2)

} // namespace

// Invisible resize grip. One of these sits on each edge/corner of the window;
// it owns the hover cursor and the mouse grab, then asks LyricWindow to apply
// the geometry so drag and resize share one clamp/save path.
class LyricWindow::ResizeHandle : public QWidget {
public:
    ResizeHandle(ResizeEdge edge, LyricWindow* window)
        : QWidget(window)
        , m_window(window)
        , m_edge(edge)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(cursorFor(edge));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_window->beginResize(m_edge, event->globalPosition().toPoint());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        m_window->updateResize(event->globalPosition().toPoint());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_window->endResize();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    static Qt::CursorShape cursorFor(ResizeEdge edge)
    {
        switch (edge) {
        case ResizeEdge::Left:
        case ResizeEdge::Right:
            return Qt::SizeHorCursor;
        case ResizeEdge::Top:
        case ResizeEdge::Bottom:
            return Qt::SizeVerCursor;
        case ResizeEdge::TopLeft:
        case ResizeEdge::BottomRight:
            return Qt::SizeFDiagCursor;
        case ResizeEdge::TopRight:
        case ResizeEdge::BottomLeft:
            return Qt::SizeBDiagCursor;
        }
        return Qt::ArrowCursor;
    }

    LyricWindow* m_window;
    ResizeEdge m_edge;
};

LyricWindow::LyricWindow(DesktopLyricConfig& config, TranslationManager& i18n)
    : QWidget(nullptr)
    , m_config(config)
    , m_i18n(i18n)
{
    Qt::WindowFlags flags = Qt::FramelessWindowHint;
    if (!m_config.isShowTaskbar())
        flags |= Qt::Tool;
    if (m_config.isAlwaysOnTop())
        flags |= Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);

    setAttribute(Qt::WA_TranslucentBackground);
    // Parity with the reference browserWindow.blur() after show: the lyric
    // window must never steal keyboard focus from the host application.
    setAttribute(Qt::WA_ShowWithoutActivating);
    // No minimum size: the window may shrink to a thin strip (user request).
    setMinimumSize(0, 0);

    // Pause-faint animation (Task D): content-level fade, never window opacity.
    m_fadeAnim = new QVariantAnimation(this);
    m_fadeAnim->setDuration(kFadeAnimationMs);
    // OutCubic is the closest Qt mapping of the reference CSS 'ease' timing.
    m_fadeAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_fadeAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_fadeFactor = value.toDouble();
                applyFade();
            });

    // Pane-fill fade (parity item 1): the pane background cross-fades between
    // rgba(0,0,0,.2) and fully transparent over 400 ms as the window locks,
    // matching the reference .lock #main { background-color: transparent } rule
    // and its @transition-theme: .4s ease.
    m_paneAnim = new QVariantAnimation(this);
    m_paneAnim->setDuration(kPaneAnimationMs);
    m_paneAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_paneAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_paneAlpha = value.toDouble();
                update(); // Repaint the pane with the interpolated alpha.
            });
    // Seed the pane alpha before the first paint: a window that starts locked
    // must not flash the shaded pane. The same-value animatePaneAlphaTo below
    // is a no-op (early exit) that keeps the retarget path exercised from the
    // start, so later lock toggles in applySetting behave identically.
    m_paneAlpha = m_config.isLock() ? 0.0 : 0.2;
    animatePaneAlphaTo(m_paneAlpha);

    // Task E: bounds persistence for compositor-native move/resize. The system
    // drives geometry and delivers a stream of move/resize events; each one
    // re-arms this timer, so saveBounds() runs once the stream goes quiet. The
    // save's config.set() re-applies geometry (applySavedPosition /
    // resizeToConfigSize) and delivers one more event; the re-armed timer then
    // fires once more and converges on a no-op set() (same values), so this
    // cannot loop.
    m_nativeOpSaveTimer = new QTimer(this);
    m_nativeOpSaveTimer->setSingleShot(true);
    m_nativeOpSaveTimer->setInterval(kNativeOpSaveDebounceMs);
    connect(m_nativeOpSaveTimer, &QTimer::timeout, this,
            [this] { saveBounds(); });

    // Quit-time bounds persistence: the host plugin exits this app with
    // QCoreApplication::quit() (--exit-on-disconnect) when its socket drops,
    // and that path never delivers a closeEvent to the window — the final
    // position would be lost if the last native move happened < 200 ms before
    // the quit (the debounce timer above never fires). aboutToQuit covers every
    // event-loop exit, closeEvent covers explicit window closes; saveBounds()
    // is idempotent, so the two may both run.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            [this] { saveBounds(); });

    // Hover-hide (Task H.1): 500 ms cursor poll (reference mouseCheckTools)
    // re-armed while desktopLyric.isHoverHide && isLock. A locked window is
    // mouse-transparent, so enter/leave events never fire; the cursor position
    // is sampled against frameGeometry() instead.
    m_hoverPollTimer = new QTimer(this);
    m_hoverPollTimer->setInterval(kHoverPollMs);
    connect(m_hoverPollTimer, &QTimer::timeout, this, &LyricWindow::pollHoverHide);

    m_contentContainer = new QWidget(this);
    m_contentContainer->setAttribute(Qt::WA_TranslucentBackground);
    m_contentContainer->setGeometry(rect());

    // The container holds every child (control bar, lyric renderer, spectrum);
    // a graphics effect over it reproduces the reference #container { opacity }
    // rule, multiplied by the static body opacity. NOTE: QGraphicsOpacityEffect
    // can glitch on translucent top-levels on some platforms; if Wayland ever
    // misrenders the fade, the fallback is per-widget painter opacity driven by
    // the same fadeFactor() (children paint through the effect today).
    m_contentEffect = new QGraphicsOpacityEffect(m_contentContainer);
    m_contentEffect->setOpacity(kBodyOpacity * m_fadeFactor);
    m_contentContainer->setGraphicsEffect(m_contentEffect);

    // Control bar floats at the top; the spectrum visualizer sits below it and
    // the stretch reserves the area for the lyric renderer (attached in a later
    // task). The visualizer is hidden unless desktopLyric.audioVisualization is
    // on; the SpectrumBridge gates its render loop off (isPlay && setting).
    m_controlBar = new ControlBar(m_config, m_i18n, m_contentContainer);
    m_spectrumWidget = new SpectrumWidget(m_config, m_contentContainer);
    m_spectrumWidget->setVisible(m_config.get(QStringLiteral("desktopLyric.audioVisualization")).toBool());
    auto* containerLayout = new QVBoxLayout(m_contentContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);
    containerLayout->addWidget(m_controlBar, 0, Qt::AlignTop);
    containerLayout->addWidget(m_spectrumWidget, 0, Qt::AlignTop);
    containerLayout->addStretch(1);

    // Order matches relayoutResizeHandles().
    m_resizeHandles[0] = new ResizeHandle(ResizeEdge::TopLeft, this);
    m_resizeHandles[1] = new ResizeHandle(ResizeEdge::TopRight, this);
    m_resizeHandles[2] = new ResizeHandle(ResizeEdge::BottomLeft, this);
    m_resizeHandles[3] = new ResizeHandle(ResizeEdge::BottomRight, this);
    m_resizeHandles[4] = new ResizeHandle(ResizeEdge::Left, this);
    m_resizeHandles[5] = new ResizeHandle(ResizeEdge::Right, this);
    m_resizeHandles[6] = new ResizeHandle(ResizeEdge::Top, this);
    m_resizeHandles[7] = new ResizeHandle(ResizeEdge::Bottom, this);
    updateResizeHandles();
    relayoutResizeHandles();

    m_alwaysOnTopTimer = new QTimer(this);
    m_alwaysOnTopTimer->setInterval(kAlwaysOnTopReassertMs);
    connect(m_alwaysOnTopTimer, &QTimer::timeout, this, [this] {
        // Some window managers (notably Wayland) drop the raised hint; this
        // re-assert is a native best-effort only.
        setWindowFlagKeepingVisible(Qt::WindowStaysOnTopHint, true);
    });

    connect(&m_config, &DesktopLyricConfig::settingChanged,
            this, &LyricWindow::applySetting);

    // Ctrl+, opens the settings dialog. ApplicationShortcut so it works even
    // though this frameless tool window rarely holds keyboard focus.
    auto* settingsShortcut = new QShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_Comma), this);
    settingsShortcut->setContext(Qt::ApplicationShortcut);
    connect(settingsShortcut, &QShortcut::activated,
            this, &LyricWindow::openSettingsDialog);

    // The gear button in the control bar is the always-visible mouse path to
    // the same dialog (the shortcut alone fails when the window has no focus,
    // e.g. on Wayland).
    connect(m_controlBar, &ControlBar::settingsRequested,
            this, &LyricWindow::openSettingsDialog);

    applyTransparentForMouseEvents();
}

void LyricWindow::openSettingsDialog()
{
    if (!m_settingsDialog)
        m_settingsDialog = new SettingsDialog(m_config, m_i18n, this);

    const bool alreadyVisible = m_settingsDialog->isVisible();

    // Keep the modeless dialog above the always-on-top lyric window. The flag
    // must be applied before show(): changing window flags on a visible widget
    // hides it. Best-effort only — some Wayland compositors ignore the hint.
    if (!alreadyVisible
        && !(m_settingsDialog->windowFlags() & Qt::WindowStaysOnTopHint)) {
        m_settingsDialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    }

    // A minimized dialog must be restored before raise() can bring it back.
    m_settingsDialog->setWindowState(m_settingsDialog->windowState()
                                     & ~Qt::WindowMinimized);

    if (alreadyVisible) {
        // The dialog can exist in a hidden state (e.g. a parent window-flag
        // flip hid it with the window); re-show it before raising, or the
        // settings stay unreachable until restart.
        if (!m_settingsDialog->isVisible())
            m_settingsDialog->show();
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }

    m_settingsDialog->show();
    m_settingsDialog->raise();
    m_settingsDialog->activateWindow();
}

void LyricWindow::faint()
{
    m_shouldBeFaint = true;

    // Reference #container.hide:not(.lock):hover — if the pointer is already
    // over the window and it is unlocked, the hover rule wins and the window
    // stays bright instead of fading underneath the cursor.
    if (!m_config.isLock() && underMouse()) {
        m_hoverOverride = true;
        animateFadeTo(1.0);
        return;
    }
    m_hoverOverride = false;
    animateFadeTo(kFaintFactor);
}

void LyricWindow::unfaint()
{
    m_shouldBeFaint = false;
    m_hoverOverride = false;
    animateFadeTo(1.0);
}

void LyricWindow::animateFadeTo(double target)
{
    // Retarget smoothly: a running fade is stopped, then restarted from the
    // last delivered frame (m_fadeFactor is kept live by valueChanged) to the
    // new target. QVariantAnimation::stop() does not emit a final valueChanged,
    // so m_fadeFactor never jumps to the old end value mid-retarget.
    if (qAbs(m_fadeFactor - target) < 1e-6) {
        m_fadeAnim->stop();
        return;
    }
    m_fadeAnim->stop();
    m_fadeAnim->setStartValue(m_fadeFactor);
    m_fadeAnim->setEndValue(target);
    m_fadeAnim->start();
}

void LyricWindow::applyFade()
{
    m_contentEffect->setOpacity(kBodyOpacity * m_fadeFactor);
    update(); // Repaint the pane with the new opacity.
}

void LyricWindow::animatePaneAlphaTo(double target)
{
    // Retarget smoothly, mirroring animateFadeTo: a running pane fade is
    // stopped, then restarted from the last delivered frame (m_paneAlpha is
    // kept live by valueChanged) to the new target. QVariantAnimation::stop()
    // does not emit a final valueChanged, so m_paneAlpha never jumps to the
    // old end value mid-retarget.
    if (qAbs(m_paneAlpha - target) < 1e-6) {
        m_paneAnim->stop();
        return;
    }
    m_paneAnim->stop();
    m_paneAnim->setStartValue(m_paneAlpha);
    m_paneAnim->setEndValue(target);
    m_paneAnim->start();
}

void LyricWindow::updateHoverHidePolling()
{
    m_hoverHideActive = m_config.get(QStringLiteral("desktopLyric.isHoverHide")).toBool()
        && m_config.isLock();

    if (!m_hoverHideActive) {
        m_hoverPollTimer->stop();
        unfaint(); // Hover-hide off (or unlocked): content returns to full.
        return;
    }
    if (!isVisible()) {
        m_hoverPollTimer->stop(); // Re-armed by showEvent.
        return;
    }
    m_hoverPollTimer->start();
    pollHoverHide(); // Sample immediately; do not wait for the first tick.
}

void LyricWindow::pollHoverHide()
{
    // Hidden (or about to be): stop polling against stale geometry instead of
    // fading under the cursor; showEvent re-arms the poll.
    if (!m_hoverHideActive || !isVisible()) {
        m_hoverPollTimer->stop();
        return;
    }
    // Same faint()/unfaint() machinery and the same 300 ms fade as pause-hide:
    // cursor over the locked window dims the content to kFaintFactor, leaving
    // it restores full opacity.
    if (isCursorInsideWindow())
        faint();
    else
        unfaint();
}

bool LyricWindow::isCursorInsideWindow() const
{
    const QRect geo = frameGeometry();
    if (!geo.isValid())
        return false;
    return geo.contains(QCursor::pos());
}

void LyricWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Reference body { opacity: .8 } compounded with the container fade: the
    // pane's static rgba(0,0,0,.2) background dims alongside the content.
    painter.setOpacity(kBodyOpacity * m_fadeFactor);

    // Parity item 1: the pane fill's alpha channel is m_paneAlpha (0.2 normal
    // / 0.0 locked) scaled to the 8-bit range, so locking the window fades the
    // background from rgba(0,0,0,.2) to fully transparent (reference
    // .lock #main { background-color: transparent }); the rgb comes from the
    // static kPaneFill.
    const QColor paneFill = QColor(kPaneFill.red(),
                                   kPaneFill.green(),
                                   kPaneFill.blue(),
                                   qRound(m_paneAlpha * 255));

    const QRectF pane = QRectF(rect());
    painter.setPen(Qt::NoPen);
    painter.setBrush(paneFill);
    painter.drawRoundedRect(pane, kRadiusBorder, kRadiusBorder);
}

void LyricWindow::enterEvent(QEnterEvent* event)
{
    // Reference #container.hide:not(.lock):hover — hovering the fainted window
    // while unlocked restores full content opacity; leaving fades it back.
    if (m_shouldBeFaint && !m_config.isLock()) {
        m_hoverOverride = true;
        animateFadeTo(1.0);
    }
    // Task F: hovering the window reveals the control bar (reference
    // #main:hover .control-bar). Fires on boundary crossing only, so the bar
    // never flickers while its own buttons are hovered (a child of this
    // window).
    m_controlBar->setHovered(true);
    QWidget::enterEvent(event);
}

void LyricWindow::leaveEvent(QEvent* event)
{
    // Only fade back if this window was brightened by hover; a plain leave
    // while playing (m_shouldBeFaint false) must not dim the window.
    if (m_hoverOverride) {
        m_hoverOverride = false;
        animateFadeTo(kFaintFactor);
    }
    m_controlBar->setHovered(false);
    QWidget::leaveEvent(event);
}

void LyricWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (!m_geometryApplied) {
        m_geometryApplied = true;
        applyWindowGeometry();
        // Startup diagnostics for the position-restore report: Wayland ignores
        // QWidget::move(), so log which platform ran, what geometry was
        // applied, and what the config wanted — if the position is still lost
        // on restart, this shows whether the platform honored the move().
        qInfo() << "[LX Lyrics] platform:" << QGuiApplication::platformName()
                << "geometry:" << geometry()
                << "config x/y:"
                << m_config.get(QStringLiteral("desktopLyric.x"))
                << m_config.get(QStringLiteral("desktopLyric.y"));
    }
    updateAlwaysOnTopLoop();
    updateHoverHidePolling(); // (Re)start the hover-hide cursor poll on show.
}

void LyricWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_contentContainer->setGeometry(rect());
    relayoutResizeHandles();

    // A compositor-native resize delivers a stream of resize events; re-arm
    // the persistence timer so the final size is flushed once it settles.
    if (m_nativeResizeActive)
        m_nativeOpSaveTimer->start();
}

void LyricWindow::closeEvent(QCloseEvent* event)
{
    // Persist the final bounds on an explicit close (WM close, session end,
    // quit button) before teardown; the quit-boundary save in the constructor
    // covers event-loop exits that never deliver this event. Idempotent.
    saveBounds();
    QWidget::closeEvent(event);
}

void LyricWindow::mousePressEvent(QMouseEvent* event)
{
    if (m_config.isLock() || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // Window-drag press (Task E routing): the reference moves the window from
    // anywhere except lyric text. Text presses never reach this handler —
    // LyricRenderer owns them for its scroll-drag — and bar buttons plus the
    // resize handles consume theirs, so any press left here (ControlBar
    // background, pane gaps, margins) is a drag region. That keeps the user
    // override (drag from the top control panel) and extends dragging to the
    // non-text lyric-area presses that escape the renderer.
    beginWindowDrag(event->globalPosition().toPoint());
    event->accept();
}

void LyricWindow::beginWindowDrag(const QPoint& globalPos)
{
    m_dragging = true;
    // A previous native move may have ended without a release event reaching
    // us; clear the stale flag so this press can still use the manual path
    // when the platform declines a new native move.
    m_nativeMoveActive = false;
    m_dragOffset = globalPos - frameGeometry().topLeft();

    // Compositor-native move: the system keeps the grab point under the cursor
    // and delivers move events while it moves the window (X11 + Wayland);
    // manual move() fights Wayland compositors. When the platform cannot move
    // natively (offscreen test platform, WMs without _NET_WM_MOVERESIZE), fall
    // back to the manual Task C drag below (mouseMoveEvent / mouseReleaseEvent).
    if (QWindow* handle = windowHandle(); handle && handle->startSystemMove()) {
        m_nativeMoveActive = true;
        m_nativeOpSaveTimer->start();
    }
}

void LyricWindow::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    // A compositor-native move drives geometry from the system; every move
    // event re-arms the persistence timer so x/y is flushed once the drag
    // settles (release events are not reliably delivered during a system move).
    if (m_nativeMoveActive)
        m_nativeOpSaveTimer->start();
}

void LyricWindow::mouseMoveEvent(QMouseEvent* event)
{
    // During a compositor-native move the system owns the geometry; a manual
    // move() here would fight it. The manual Task C fallback below only runs
    // when the platform could not start a native move.
    if (m_nativeMoveActive || !m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    QPoint topLeft = event->globalPosition().toPoint() - m_dragOffset;
    const QRect available = primaryScreenAvailableGeometry();
    if (m_config.isLockScreen() && available.isValid())
        topLeft = clampedTopLeft(topLeft, size(), available);

    move(topLeft);
    event->accept();
}

void LyricWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        // Some platforms deliver the release even after a native move; stop
        // the pending debounce and persist immediately. On platforms that do
        // not, moveEvent's re-armed timer performs this same save.
        m_nativeMoveActive = false;
        m_nativeOpSaveTimer->stop();
        saveBounds(); // Saves x/y/size and flushes them to disk immediately.
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void LyricWindow::applySetting(const QString& key, const QVariant& value)
{
    if (key == QStringLiteral("desktopLyric.isLock")) {
        if (value.toBool() && m_resizing) {
            m_resizing = false; // Lock can flip mid-drag; stop resizing cleanly.
            m_nativeResizeActive = false;
            m_nativeOpSaveTimer->stop();
            saveBounds();
        }
        applyTransparentForMouseEvents();
        updateResizeHandles();
        updateHoverHidePolling(); // Hover-hide only runs while locked.
        // Parity item 1: locking fades the pane background to fully transparent
        // (reference .lock #main { background-color: transparent }); unlocking
        // restores the rgba(0,0,0,.2) shade. Both cross-fade over 400 ms.
        animatePaneAlphaTo(m_config.isLock() ? 0.0 : 0.2);
    } else if (key == QStringLiteral("desktopLyric.isAlwaysOnTop")) {
        setWindowFlagKeepingVisible(Qt::WindowStaysOnTopHint, value.toBool());
        updateAlwaysOnTopLoop();
    } else if (key == QStringLiteral("desktopLyric.isShowTaskbar")) {
        setWindowFlagKeepingVisible(Qt::Tool, value.toBool());
    } else if (key == QStringLiteral("desktopLyric.isLockScreen")) {
        if (value.toBool())
            clampToAvailableGeometry();
    } else if (key == QStringLiteral("desktopLyric.isHoverHide")) {
        updateHoverHidePolling(); // Re-arm/stop the cursor poll on toggle.
    } else if (key == QStringLiteral("desktopLyric.width")
               || key == QStringLiteral("desktopLyric.height")) {
        resizeToConfigSize();
    } else if (key == QStringLiteral("desktopLyric.x")
               || key == QStringLiteral("desktopLyric.y")) {
        applySavedPosition();
    } else if (key == QStringLiteral("desktopLyric.audioVisualization")) {
        m_spectrumWidget->setVisible(value.toBool());
    }
}

void LyricWindow::applyWindowGeometry()
{
    int width = m_config.get(QStringLiteral("desktopLyric.width")).toInt();
    int height = m_config.get(QStringLiteral("desktopLyric.height")).toInt();

    const QRect available = primaryScreenAvailableGeometry();
    if (!available.isValid()) {
        resize(width, height);
        return; // No screen info: let the window manager place us.
    }

    if (m_config.isLockScreen()) {
        width = qMin(width, available.width());
        height = qMin(height, available.height());
    }

    const QVariant storedX = m_config.get(QStringLiteral("desktopLyric.x"));
    const QVariant storedY = m_config.get(QStringLiteral("desktopLyric.y"));
    QPoint topLeft;
    if (storedX.isValid() && storedY.isValid()) {
        topLeft = QPoint(storedX.toInt(), storedY.toInt());
    } else {
        // Null coordinates: center on the primary screen's available area.
        topLeft = QPoint(available.x() + (available.width() - width) / 2,
                         available.y() + (available.height() - height) / 2);
    }

    // A stored position is trusted only while it is still visible on some
    // screen; one that has drifted off-screen (monitor unplugged, resolution
    // change) is rescued back into the primary screen's available area so the
    // window is always findable on restart.
    if (!intersectsAnyScreen(topLeft, QSize(width, height)))
        topLeft = clampedTopLeft(topLeft, QSize(width, height), available);

    resize(width, height);
    move(topLeft);
}

void LyricWindow::applySavedPosition()
{
    const QVariant storedX = m_config.get(QStringLiteral("desktopLyric.x"));
    const QVariant storedY = m_config.get(QStringLiteral("desktopLyric.y"));
    if (storedX.isValid() && storedY.isValid()) {
        move(storedX.toInt(), storedY.toInt());
        return;
    }
    applyWindowGeometry(); // A coordinate was reset to null: recenter.
}

void LyricWindow::resizeToConfigSize()
{
    int width = m_config.get(QStringLiteral("desktopLyric.width")).toInt();
    int height = m_config.get(QStringLiteral("desktopLyric.height")).toInt();

    const QRect available = primaryScreenAvailableGeometry();
    if (m_config.isLockScreen() && available.isValid()) {
        width = qMin(width, available.width());
        height = qMin(height, available.height());
    }

    resize(width, height);
}

void LyricWindow::clampToAvailableGeometry()
{
    const QRect available = primaryScreenAvailableGeometry();
    if (!available.isValid())
        return;
    move(clampedTopLeft(pos(), size(), available));
}

void LyricWindow::applyTransparentForMouseEvents()
{
    // A locked lyric window is ALWAYS click-through: the pointer passes
    // straight through to whatever is underneath. isHoverHide must not opt out
    // — hover-hide runs off the 500 ms cursor poll (m_hoverPollTimer), not
    // mouse events, so a locked window never needs to consume pointer input.
    const bool locked = m_config.isLock();
    setAttribute(Qt::WA_TransparentForMouseEvents, locked);

    // The window flag is the documented input-transparency mechanism ("Window
    // is transparent to input events, that is, all input events are passed to
    // the window behind it") and covers platforms where the attribute alone is
    // unreliable. Best-effort on Wayland: KWin rules cannot express input
    // transparency, so click-through depends on the compositor honoring either
    // mechanism. The hide/show inside setWindowFlagKeepingVisible preserves an
    // open settings dialog automatically (Bug 3 fix).
    setWindowFlagKeepingVisible(Qt::WindowTransparentForInput, locked);
}

void LyricWindow::updateAlwaysOnTopLoop()
{
    if (m_config.isAlwaysOnTop())
        m_alwaysOnTopTimer->start();
    else
        m_alwaysOnTopTimer->stop();
}

void LyricWindow::setWindowFlagKeepingVisible(Qt::WindowType flag, bool on)
{
    // Early exit: QWidget::setWindowFlags already no-ops on an unchanged flag,
    // but this guard makes the intent explicit — an unchanged flag must never
    // trigger the hide/show cycle below (the 500 ms always-on-top re-assert
    // would otherwise flicker the window and re-raise it every tick).
    if (windowFlags().testFlag(flag) == on)
        return;

    // Changing window flags hides the widget — and with it any modeless child
    // dialog (the settings dialog). Qt does NOT re-show the child when the
    // parent is re-shown, so it must be brought back explicitly.
    const bool dialogWasVisible = m_settingsDialog && m_settingsDialog->isVisible();
    const bool wasVisible = isVisible();
    setWindowFlag(flag, on); // Changing window flags hides the widget.
    if (wasVisible) {
        show();
        if (dialogWasVisible) {
            m_settingsDialog->show();
            m_settingsDialog->raise();
        }
    }
}

void LyricWindow::saveBounds()
{
    // Startup guard: before showEvent applies the config geometry,
    // pos()/size() are pre-geometry defaults that would overwrite the persisted
    // position with a stale one. Safe to call from any exit boundary after the
    // window has been shown; a no-op before it.
    if (!m_geometryApplied)
        return;

    // Snapshot the whole bounds first: each config.set() below synchronously
    // re-applies geometry (applySetting -> applySavedPosition/resizeToConfigSize),
    // so reading width()/height() after the first set() would capture a
    // partially re-applied size and corrupt the saved values.
    const int x = pos().x();
    const int y = pos().y();
    const int w = width();
    const int h = height();

    m_config.set(QStringLiteral("desktopLyric.x"), x);
    m_config.set(QStringLiteral("desktopLyric.y"), y);
    m_config.set(QStringLiteral("desktopLyric.width"), w);
    m_config.set(QStringLiteral("desktopLyric.height"), h);
    // Persist immediately: the 500 ms debounced write would be lost on a crash
    // or kill right after a drag/resize, and the position must survive any
    // abrupt exit to be remembered on the next start.
    m_config.flush();
}

QRect LyricWindow::primaryScreenAvailableGeometry() const
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect();
}

bool LyricWindow::intersectsAnyScreen(const QPoint& topLeft, const QSize& size) const
{
    const QRect windowRect(topLeft, size);
    const auto screens = QGuiApplication::screens();
    for (const QScreen* screen : screens) {
        if (screen->availableGeometry().intersects(windowRect))
            return true;
    }
    return false;
}

QPoint LyricWindow::clampedTopLeft(const QPoint& topLeft, const QSize& windowSize, const QRect& available) const
{
    const int maxX = available.x() + qMax(0, available.width() - windowSize.width());
    const int maxY = available.y() + qMax(0, available.height() - windowSize.height());
    return QPoint(qBound(available.x(), topLeft.x(), maxX),
                  qBound(available.y(), topLeft.y(), maxY));
}

void LyricWindow::updateResizeHandles()
{
    const bool visible = !m_config.isLock();
    for (ResizeHandle* handle : m_resizeHandles)
        handle->setVisible(visible);
}

void LyricWindow::relayoutResizeHandles()
{
    const int h = kResizeHandleSize;
    const int w = qMax(0, width());
    const int hh = qMax(0, height());
    // With no minimum size the window can shrink below 2 * handle width; the
    // side/top strips must never get a negative dimension, and the 8 px corner
    // squares stay put so an edge is always grabbable.
    const int sideHeight = qMax(0, hh - 2 * h);
    const int topWidth = qMax(0, w - 2 * h);

    // Order matches the constructor's m_resizeHandles[] assignment.
    m_resizeHandles[0]->setGeometry(0, 0, h, h);              // TopLeft
    m_resizeHandles[1]->setGeometry(w - h, 0, h, h);          // TopRight
    m_resizeHandles[2]->setGeometry(0, hh - h, h, h);         // BottomLeft
    m_resizeHandles[3]->setGeometry(w - h, hh - h, h, h);     // BottomRight
    m_resizeHandles[4]->setGeometry(0, h, h, sideHeight);     // Left
    m_resizeHandles[5]->setGeometry(w - h, h, h, sideHeight); // Right
    m_resizeHandles[6]->setGeometry(h, 0, topWidth, h);       // Top
    m_resizeHandles[7]->setGeometry(h, hh - h, topWidth, h);  // Bottom
}

void LyricWindow::beginResize(ResizeEdge edge, const QPoint& globalPos)
{
    if (m_config.isLock())
        return;
    m_resizeEdge = edge;
    m_resizing = true;
    // A previous native resize may have ended without a release event reaching
    // us; clear the stale flag so this press can still use the manual path
    // when the platform declines a new native resize.
    m_nativeResizeActive = false;
    m_resizeStartMouse = globalPos;
    m_resizeStartGeometry = frameGeometry();

    // Compositor-native resize: the system drives the requested edges while Qt
    // delivers the resize event stream (X11 + Wayland; manual setGeometry()
    // fights Wayland compositors). When the platform cannot resize natively
    // (offscreen test platform), the manual Task C path — the handle's
    // mouseMoveEvent -> updateResize() and release -> endResize() — stays.
    if (QWindow* handle = windowHandle(); handle && handle->startSystemResize(nativeEdgesFor(edge))) {
        m_nativeResizeActive = true;
        m_nativeOpSaveTimer->start();
    }
}

Qt::Edges LyricWindow::nativeEdgesFor(ResizeEdge edge) const
{
    switch (edge) {
    case ResizeEdge::TopLeft:
        return Qt::TopEdge | Qt::LeftEdge;
    case ResizeEdge::TopRight:
        return Qt::TopEdge | Qt::RightEdge;
    case ResizeEdge::BottomLeft:
        return Qt::BottomEdge | Qt::LeftEdge;
    case ResizeEdge::BottomRight:
        return Qt::BottomEdge | Qt::RightEdge;
    case ResizeEdge::Left:
        return Qt::LeftEdge;
    case ResizeEdge::Right:
        return Qt::RightEdge;
    case ResizeEdge::Top:
        return Qt::TopEdge;
    case ResizeEdge::Bottom:
        return Qt::BottomEdge;
    }
    return Qt::Edges();
}

void LyricWindow::updateResize(const QPoint& globalPos)
{
    if (m_config.isLock()) {
        m_resizing = false;
        return;
    }
    // A native resize owns the geometry; the manual delta math must not run.
    if (!m_resizing || m_nativeResizeActive)
        return;
    setGeometry(resizeGeometryFor(globalPos));
}

void LyricWindow::endResize()
{
    if (!m_resizing)
        return;
    m_resizing = false;
    // Some platforms deliver the release even after a native resize; stop the
    // pending debounce and persist immediately. On platforms that do not,
    // resizeEvent's re-armed timer performs this same save.
    m_nativeResizeActive = false;
    m_nativeOpSaveTimer->stop();
    saveBounds(); // Saves x/y/size and flushes them to disk immediately.
}

QRect LyricWindow::resizeGeometryFor(const QPoint& globalPos) const
{
    const int dx = globalPos.x() - m_resizeStartMouse.x();
    const int dy = globalPos.y() - m_resizeStartMouse.y();

    QRect geo = m_resizeStartGeometry;
    switch (m_resizeEdge) {
    case ResizeEdge::Left:
        geo.setLeft(geo.left() + dx);
        break;
    case ResizeEdge::Right:
        geo.setRight(geo.right() + dx);
        break;
    case ResizeEdge::Top:
        geo.setTop(geo.top() + dy);
        break;
    case ResizeEdge::Bottom:
        geo.setBottom(geo.bottom() + dy);
        break;
    case ResizeEdge::TopLeft:
        geo.setLeft(geo.left() + dx);
        geo.setTop(geo.top() + dy);
        break;
    case ResizeEdge::TopRight:
        geo.setRight(geo.right() + dx);
        geo.setTop(geo.top() + dy);
        break;
    case ResizeEdge::BottomLeft:
        geo.setLeft(geo.left() + dx);
        geo.setBottom(geo.bottom() + dy);
        break;
    case ResizeEdge::BottomRight:
        geo.setRight(geo.right() + dx);
        geo.setBottom(geo.bottom() + dy);
        break;
    }

    // No minimum size: an edge may collapse the window to a zero-wide strip,
    // but the rect must never invert (left <= right, top <= bottom). The
    // moving edge stops at its opposite edge, like a normal window.
    const auto keepNonInverted = [this, &geo] {
        if (geo.width() < 0) {
            if (m_resizeEdge == ResizeEdge::Left
                || m_resizeEdge == ResizeEdge::TopLeft
                || m_resizeEdge == ResizeEdge::BottomLeft)
                geo.setLeft(geo.right());
            else
                geo.setRight(geo.left());
        }
        if (geo.height() < 0) {
            if (m_resizeEdge == ResizeEdge::Top
                || m_resizeEdge == ResizeEdge::TopLeft
                || m_resizeEdge == ResizeEdge::TopRight)
                geo.setTop(geo.bottom());
            else
                geo.setBottom(geo.top());
        }
    };
    keepNonInverted();

    if (m_config.isLockScreen()) {
        const QRect available = primaryScreenAvailableGeometry();
        if (available.isValid()) {
            // Every edge stays inside the available area. Re-snap afterwards:
            // clamping one edge can push a rect that sits fully off one side
            // into an inverted shape again.
            if (geo.left() < available.left())
                geo.setLeft(available.left());
            if (geo.right() > available.right())
                geo.setRight(available.right());
            if (geo.top() < available.top())
                geo.setTop(available.top());
            if (geo.bottom() > available.bottom())
                geo.setBottom(available.bottom());
            keepNonInverted();
        }
    }
    return geo;
}
