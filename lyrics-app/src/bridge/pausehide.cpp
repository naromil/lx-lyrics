/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "bridge/pausehide.h"

#include "config/desktoplyricconfig.h"

namespace {

constexpr int kPauseFaintDelayMs = 200; // Mirrors the reference usePauseHide.ts.

} // namespace

PauseHide::PauseHide(DesktopLyricConfig& config, QObject* parent)
  : QObject(parent)
  , m_config(config)
  , m_pauseHideEnabled(m_config.get(QStringLiteral("desktopLyric.pauseHide")).toBool())
{
  m_faintTimer.setSingleShot(true);
  m_faintTimer.setInterval(kPauseFaintDelayMs);
  connect(&m_faintTimer, &QTimer::timeout, this, [this] {
    m_isFainted = true;
    emit faintRequested();
  });

  connect(&m_config, &DesktopLyricConfig::settingChanged, this, &PauseHide::applySetting);

  // Reference usePauseHide.ts installs its isPlay watcher with immediate:true
  // and store/state.ts defaults isPlay to false: a freshly loaded window is
  // treated as paused and faints after the delay. Mirror that so a restarted
  // display stays dimmed while the music is paused even before the first
  // set_info/set_status arrives (and if the host is unreachable); the first
  // play-state message corrects it. applyPlayState() early-returns while the
  // setting is disabled.
  applyPlayState();
}

void PauseHide::setPlayState(bool isPlay)
{
  // Record even while disabled so a later enable re-applies the current
  // state (the reference outer watcher re-applies isPlay on every enable).
  m_isPlay = isPlay;
  applyPlayState();
}

void PauseHide::applyPlayState()
{
  if (!m_pauseHideEnabled)
    return; // Setting disabled: the reference installs no watcher at all.

  cancelPendingFaint();
  if (m_isPlay) {
    unfaintIfFainted();
    return;
  }
  m_faintTimer.start();
}

void PauseHide::applySetting(const QString& key, const QVariant& value)
{
  if (key != QStringLiteral("desktopLyric.pauseHide"))
    return;

  const bool enabled = value.toBool();
  if (enabled == m_pauseHideEnabled)
    return;

  m_pauseHideEnabled = enabled;
  if (enabled) {
    // Enabled mid-session: re-apply the current play state like the
    // reference outer immediate watcher — paused faints after the delay,
    // playing unfaints (a no-op when not fainted).
    applyPlayState();
    return;
  }

  // Disabled mid-pause: cancel a pending faint and restore full opacity.
  cancelPendingFaint();
  unfaintIfFainted();
}

void PauseHide::cancelPendingFaint()
{
  m_faintTimer.stop();
}

void PauseHide::unfaintIfFainted()
{
  if (!m_isFainted)
    return;
  m_isFainted = false;
  emit unfaintRequested();
}
