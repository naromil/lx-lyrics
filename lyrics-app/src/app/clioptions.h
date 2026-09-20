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
  // --player-feed: the app was spawned as the direct child of a player-side
  // adapter and speaks protocol v2 over stdin/stdout.
  bool playerFeed = false;
  // --demo: standalone self-fed mode (fake track + synthetic spectrum), no host.
  bool demo = false;
};

// Parses the app's command-line arguments. Unknown arguments are ignored.
// Precedence: --player-feed > --demo > inert window.
inline CliOptions parseCliOptions(const QStringList& args)
{
  CliOptions options;

  for (const QString& arg : args) {
    if (arg == QStringLiteral("--player-feed")) {
      options.playerFeed = true;
    } else if (arg == QStringLiteral("--demo")) {
      options.demo = true;
    }
  }

  return options;
}
