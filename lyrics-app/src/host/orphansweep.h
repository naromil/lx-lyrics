/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

/**
 * Startup cleanup for leftover `lx-lyrics-app --player-feed` processes whose
 * player-side adapter died before it could close their stdin (protocol.md §2),
 * so the desktop lyric window would otherwise stay on screen forever.
 *
 * The primary path is now the in-process self-watch (host/parentwatch.h): a
 * feed child quits within ~1 s when its own parent dies, EOF or not. This
 * cross-process sweep only clears pre-existing orphans (sessions started by an
 * older build or a legacy adapter) when a new feed session starts.
 *
 * A process is a candidate only when all of these hold: it is not this process
 * or this process's parent, it has our real uid, its argv carries
 * `--player-feed`, its argv[0] basename or /proc/<pid>/exe basename is
 * `lx-lyrics-app`, and it is an orphan — its parent is pid 1, is gone, is a
 * zombie, or is the systemd user manager (a child subreaper, so a killed
 * player's child is reparented there instead of to pid 1). Anything whose
 * parent is alive is never touched. Each candidate gets SIGTERM, up to one
 * second to leave, then SIGKILL.
 *
 * Returns the number of processes that were signalled. Unreadable /proc
 * entries, disappearing processes and a missing /proc (non-Linux) are all
 * non-fatal: the sweep never fails the app.
 *
 * Plain POSIX C++ (Qt is used only for the log line) so it stays testable
 * without a host.
 */
int reapOrphanFeeds();
