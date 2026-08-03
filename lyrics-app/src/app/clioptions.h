/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QString>
#include <QStringList>

// Command-line contract defined in docs/protocol.md §3.
struct CliOptions {
    QString wsUrl;                 // --ws=ws://127.0.0.1:PORT (host WebSocket URL)
    bool exitOnDisconnect = false; // --exit-on-disconnect
    bool demo = false;             // --demo (standalone self-fed mode)
};

// Parses the app's command-line arguments. Unknown arguments are ignored.
inline CliOptions parseCliOptions(const QStringList& args)
{
    CliOptions options;

    for (const QString& arg : args) {
        if (arg.startsWith(QStringLiteral("--ws="))) {
            options.wsUrl = arg.sliced(5);
        } else if (arg == QStringLiteral("--exit-on-disconnect")) {
            options.exitOnDisconnect = true;
        } else if (arg == QStringLiteral("--demo")) {
            options.demo = true;
        }
    }

    return options;
}
