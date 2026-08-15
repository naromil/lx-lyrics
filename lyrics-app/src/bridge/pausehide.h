/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <QVariant>

class DesktopLyricConfig;

// Port of the reference usePauseHide.ts: when the host reports that playback
// has stopped (isPlay == false) and the `desktopLyric.pauseHide` setting is
// enabled, the window is dimmed (fainted) after a short delay so brief pauses
// do not flicker. The window is restored (unfainted) immediately when playback
// resumes. This module never hides or shows the window — it only requests a
// change of opacity.
//
// Any state message that conveys the play boolean feeds this via
// setPlayState(): set_info/set_status carry isPlay; set_play implies true;
// set_pause/set_stop imply false.
class PauseHide : public QObject {
  Q_OBJECT

public:
  explicit PauseHide(DesktopLyricConfig& config, QObject* parent = nullptr);

  // Feeds the current play boolean. The state is recorded even while
  // pauseHide is disabled so a later enable applies the current state.
  // When pauseHide is enabled, a false schedules faintRequested() after the
  // 200 ms delay; a true cancels any pending faint and emits
  // unfaintRequested() immediately if the window was fainted by this module.
  void setPlayState(bool isPlay);

signals:
  void faintRequested();
  void unfaintRequested();

private:
  void applySetting(const QString& key, const QVariant& value);
  void applyPlayState();
  void cancelPendingFaint();
  void unfaintIfFainted();

  DesktopLyricConfig& m_config;
  QTimer m_faintTimer;
  bool m_pauseHideEnabled = false;
  bool m_isPlay = false; // Mirrors store/state.ts isPlay = ref(false).
  bool m_isFainted = false;
};
