/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QObject>

#include <sys/types.h>

class QTimer;

/**
 * In-process watchdog for the `--player-feed` quit path: the player-side
 * adapter that spawned this app is its direct parent, and EOF on stdin is the
 * normal quit trigger (docs/protocol.md §2). A player that dies without
 * closing the pipe — killed, crashed, its adapter torn down mid-write — leaves
 * the pipe open, so no EOF ever arrives and the window would stay up forever.
 *
 * The watchdog compares the current parent pid with the one captured at
 * start(): a living parent cannot change, so any difference means the parent
 * died and this process was reparented (to pid 1, `systemd --user`, or another
 * subreaper). Steady state is one getppid() syscall per timer tick; no /proc
 * enumeration, no other process is touched.
 *
 * A process that is already orphaned when start() runs (the parent died
 * between fork/exec and main()) also counts: getppid() <= 1, or the parent's
 * /proc/<pid>/comm is `systemd` — the user manager is a child subreaper, so
 * the app lands there rather than on pid 1. That one-time comm read is the
 * only /proc access.
 *
 * `orphaned()` is emitted exactly once; the timer then stops. Nothing is sent
 * to the host: a player-driven end must not produce `close_requested`, and the
 * app just quits.
 */
class ParentWatch : public QObject {
  Q_OBJECT

public:
  explicit ParentWatch(QObject* parent = nullptr);

  // Captures the current parent pid, checks it once, and (unless already
  // orphaned) polls it every `intervalMs`.
  void start(int intervalMs = 1000);

signals:
  void orphaned();

private:
  bool initiallyOrphaned() const;
  void check();
  void reportOrphaned();

  pid_t m_parentPid = 0;
  bool m_started = false;
  bool m_reported = false;
  QTimer* m_timer = nullptr;
};
