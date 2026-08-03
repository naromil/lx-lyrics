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
// enabled, the window is hidden after a short delay so brief pauses do not
// flicker. The window is shown again immediately when playback resumes.
//
// Any state message that conveys the play boolean feeds this via
// setPlayState(): set_info/set_status carry isPlay; set_play implies true;
// set_pause/set_stop imply false.
class PauseHide : public QObject {
    Q_OBJECT

public:
    explicit PauseHide(DesktopLyricConfig& config, QObject* parent = nullptr);

    // Feeds the current play boolean. When pauseHide is enabled, a false
    // schedules hideRequested() after the 200 ms delay; a true cancels any
    // pending hide and emits showRequested() immediately if the window was
    // hidden by this module.
    void setPlayState(bool isPlay);

signals:
    void hideRequested();
    void showRequested();

private:
    void applySetting(const QString& key, const QVariant& value);
    void cancelPendingHide();
    void revealIfHidden();

    DesktopLyricConfig& m_config;
    QTimer m_hideTimer;
    bool m_pauseHideEnabled = false;
    bool m_isHidden = false;
};
