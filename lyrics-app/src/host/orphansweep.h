/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QtGlobal>

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
 * second to leave, then SIGKILL. The pid alone is not an identity: the sweep
 * records the candidate's kernel start time during the scan and re-checks it
 * before SIGTERM, before the SIGKILL escalation and on every wait-loop tick,
 * so a candidate that exits and whose pid is reused is never signalled.
 *
 * Returns the number of processes that were signalled. Unreadable /proc
 * entries, disappearing processes and a missing /proc (non-Linux) are all
 * non-fatal: the sweep never fails the app.
 *
 * Plain POSIX C++ (Qt supplies only the log line and the fixed-width helper
 * types) so it stays testable without a host.
 */
int reapOrphanFeeds();

/**
 * Reads @p pid's kernel start time — field 22 of /proc/<pid>/stat, in clock
 * ticks since boot — into @p starttime. Combined with the pid this identifies
 * one process instance for the lifetime of the kernel, so a caller can tell a
 * live candidate from a different process that inherited its pid.
 *
 * Returns false when the process is gone, is a zombie whose entry vanished
 * mid-read, or the entry cannot be parsed. Exposed for the sweep's own guard
 * and for tests.
 */
bool readProcessStartTime(qint64 pid, quint64* starttime);

/**
 * True only when @p pid is currently readable AND still has @p starttime, i.e.
 * the process captured earlier has not exited. A mismatch means the pid now
 * belongs to an unrelated process; an unreadable entry means it is gone. Both
 * count as "not the candidate", so the caller must not signal it.
 */
bool processStartTimeMatches(qint64 pid, quint64 starttime);
