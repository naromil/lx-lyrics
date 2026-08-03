/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "window/lyricwindow.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"
#include "renderer/controlbar.h"
#include "renderer/spectrumwidget.h"
#include "settings/settingsdialog.h"

namespace {

constexpr int kCornerRadius = 12;
constexpr int kAlwaysOnTopReassertMs = 500;

constexpr int kResizeHandleSize = 8; // 8 px edge/corner hit regions.
constexpr int kMinWindowWidth = 38;
constexpr int kMinWindowHeight = 38;

const QColor kPaneFill(0, 0, 0, 51);       // rgba(0, 0, 0, 0.2)
const QColor kPaneBorder(255, 255, 255, 45); // Thin light border.

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

    m_contentContainer = new QWidget(this);
    m_contentContainer->setAttribute(Qt::WA_TranslucentBackground);
    m_contentContainer->setGeometry(rect());

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

    applyTransparentForMouseEvents();
}

void LyricWindow::openSettingsDialog()
{
    if (!m_settingsDialog)
        m_settingsDialog = new SettingsDialog(m_config, m_i18n, this);
    if (m_settingsDialog->isVisible()) {
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    m_settingsDialog->show();
    m_settingsDialog->raise();
    m_settingsDialog->activateWindow();
}

void LyricWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF pane = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(kPaneBorder, 1.0));
    painter.setBrush(kPaneFill);
    painter.drawRoundedRect(pane, kCornerRadius, kCornerRadius);
}

void LyricWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (!m_geometryApplied) {
        m_geometryApplied = true;
        applyWindowGeometry();
    }
    updateAlwaysOnTopLoop();
}

void LyricWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_contentContainer->setGeometry(rect());
    relayoutResizeHandles();
}

void LyricWindow::mousePressEvent(QMouseEvent* event)
{
    if (m_config.isLock()) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LyricWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
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
        saveBounds(); // config.save() debounces the actual write.
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
            saveBounds();
        }
        applyTransparentForMouseEvents();
        updateResizeHandles();
    } else if (key == QStringLiteral("desktopLyric.isAlwaysOnTop")) {
        setWindowFlagKeepingVisible(Qt::WindowStaysOnTopHint, value.toBool());
        updateAlwaysOnTopLoop();
    } else if (key == QStringLiteral("desktopLyric.isShowTaskbar")) {
        setWindowFlagKeepingVisible(Qt::Tool, value.toBool());
    } else if (key == QStringLiteral("desktopLyric.isLockScreen")) {
        if (value.toBool())
            clampToAvailableGeometry();
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

    if (m_config.isLockScreen())
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
    const bool ignoreMouse = m_config.isLock()
        && !m_config.get(QStringLiteral("desktopLyric.isHoverHide")).toBool();
    setAttribute(Qt::WA_TransparentForMouseEvents, ignoreMouse);
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
    const bool wasVisible = isVisible();
    setWindowFlag(flag, on); // Changing window flags hides the widget.
    if (wasVisible)
        show();
}

void LyricWindow::saveBounds()
{
    m_config.set(QStringLiteral("desktopLyric.x"), pos().x());
    m_config.set(QStringLiteral("desktopLyric.y"), pos().y());
    m_config.set(QStringLiteral("desktopLyric.width"), width());
    m_config.set(QStringLiteral("desktopLyric.height"), height());
}

QRect LyricWindow::primaryScreenAvailableGeometry() const
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry() : QRect();
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
    const int w = width();
    const int hh = height();

    // Order matches the constructor's m_resizeHandles[] assignment.
    m_resizeHandles[0]->setGeometry(0, 0, h, h);              // TopLeft
    m_resizeHandles[1]->setGeometry(w - h, 0, h, h);          // TopRight
    m_resizeHandles[2]->setGeometry(0, hh - h, h, h);         // BottomLeft
    m_resizeHandles[3]->setGeometry(w - h, hh - h, h, h);     // BottomRight
    m_resizeHandles[4]->setGeometry(0, h, h, hh - 2 * h);     // Left
    m_resizeHandles[5]->setGeometry(w - h, h, h, hh - 2 * h); // Right
    m_resizeHandles[6]->setGeometry(h, 0, w - 2 * h, h);      // Top
    m_resizeHandles[7]->setGeometry(h, hh - h, w - 2 * h, h); // Bottom
}

void LyricWindow::beginResize(ResizeEdge edge, const QPoint& globalPos)
{
    if (m_config.isLock())
        return;
    m_resizeEdge = edge;
    m_resizing = true;
    m_resizeStartMouse = globalPos;
    m_resizeStartGeometry = frameGeometry();
}

void LyricWindow::updateResize(const QPoint& globalPos)
{
    if (m_config.isLock()) {
        m_resizing = false;
        return;
    }
    if (!m_resizing)
        return;
    setGeometry(resizeGeometryFor(globalPos));
}

void LyricWindow::endResize()
{
    if (!m_resizing)
        return;
    m_resizing = false;
    saveBounds(); // config.save() debounces the actual write.
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

    // Enforce the minimum first so shrinking below 38 px keeps the opposite
    // edge fixed instead of letting the window collapse.
    if (geo.width() < kMinWindowWidth) {
        if (m_resizeEdge == ResizeEdge::Left
            || m_resizeEdge == ResizeEdge::TopLeft
            || m_resizeEdge == ResizeEdge::BottomLeft)
            geo.setLeft(geo.right() - kMinWindowWidth + 1);
        else
            geo.setRight(geo.left() + kMinWindowWidth - 1);
    }
    if (geo.height() < kMinWindowHeight) {
        if (m_resizeEdge == ResizeEdge::Top
            || m_resizeEdge == ResizeEdge::TopLeft
            || m_resizeEdge == ResizeEdge::TopRight)
            geo.setTop(geo.bottom() - kMinWindowHeight + 1);
        else
            geo.setBottom(geo.top() + kMinWindowHeight - 1);
    }

    if (m_config.isLockScreen()) {
        const QRect available = primaryScreenAvailableGeometry();
        if (available.isValid()) {
            // Every edge stays inside the available area; on any screen at
            // least 38 px wide the minimum-size clamp above still holds.
            if (geo.left() < available.left())
                geo.setLeft(available.left());
            if (geo.right() > available.right())
                geo.setRight(available.right());
            if (geo.top() < available.top())
                geo.setTop(available.top());
            if (geo.bottom() > available.bottom())
                geo.setBottom(available.bottom());
        }
    }
    return geo;
}
