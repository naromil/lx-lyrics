/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "bridge/pausehide.h"

#include "config/desktoplyricconfig.h"

namespace {

constexpr int kPauseHideDelayMs = 200; // Mirrors the reference usePauseHide.ts.

} // namespace

PauseHide::PauseHide(DesktopLyricConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_pauseHideEnabled(m_config.get(QStringLiteral("desktopLyric.pauseHide")).toBool())
{
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(kPauseHideDelayMs);
    connect(&m_hideTimer, &QTimer::timeout, this, [this] {
        m_isHidden = true;
        emit hideRequested();
    });

    connect(&m_config, &DesktopLyricConfig::settingChanged,
            this, &PauseHide::applySetting);
}

void PauseHide::setPlayState(bool isPlay)
{
    if (!m_pauseHideEnabled)
        return; // Setting disabled: the reference installs no watcher at all.

    cancelPendingHide();
    if (isPlay) {
        revealIfHidden();
        return;
    }
    m_hideTimer.start();
}

void PauseHide::applySetting(const QString& key, const QVariant& value)
{
    if (key != QStringLiteral("desktopLyric.pauseHide"))
        return;

    const bool enabled = value.toBool();
    if (enabled == m_pauseHideEnabled)
        return;

    m_pauseHideEnabled = enabled;
    if (enabled)
        return; // The next setPlayState applies the new behavior.

    // Disabled mid-pause: cancel a pending hide and reveal a hidden window.
    cancelPendingHide();
    revealIfHidden();
}

void PauseHide::cancelPendingHide()
{
    m_hideTimer.stop();
}

void PauseHide::revealIfHidden()
{
    if (!m_isHidden)
        return;
    m_isHidden = false;
    emit showRequested();
}
