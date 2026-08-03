/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <memory>
#include <utility>

#include "clioptions.h"
#include "bridge/pausehide.h"
#include "bridge/wsclient.h"
#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"
#include "app/spectrumbridge.h"

// Shared application state. Later tasks attach the lyric renderer here.
class AppContext : public QObject {
    Q_OBJECT

public:
    explicit AppContext(CliOptions cli, QObject* parent = nullptr)
        : QObject(parent)
        , cli(std::move(cli))
    {
    }

    CliOptions cli;
    DesktopLyricConfig config;
    // Attached in main() after config.load() so a persisted common.langId is
    // applied at startup; parented to AppContext for app-wide access.
    std::unique_ptr<TranslationManager> i18n;
    QPointer<QWidget> mainWindow;

    // WebSocket bridge to the host plugin. Created in main() only when the
    // CLI supplies --ws; signals are wired into the app pipeline there
    // (LyricController + PauseHide + SpectrumBridge).
    std::unique_ptr<WsClient> wsClient;
    // Pause-faint watcher (port of usePauseHide.ts). Always present so the
    // window honors desktopLyric.pauseHide regardless of the connection mode.
    std::unique_ptr<PauseHide> pauseHide;
    // Spectrum-only wiring for the visualizer (task 2.11): forwards analyser
    // frames to the SpectrumWidget and gates its loop off play + the
    // audioVisualization setting. Created in main() only in --ws mode.
    std::unique_ptr<SpectrumBridge> spectrumBridge;
};
