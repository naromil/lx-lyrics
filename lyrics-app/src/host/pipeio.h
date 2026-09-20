/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QByteArray>
#include <QIODevice>

class QSocketNotifier;

// The app's end of the two pipes its parent (the player-side adapter) wired up
// (docs/protocol.md §2).
//
// QFile CANNOT be used for either direction: Qt's file engine neither emits
// readyRead nor reports bytesAvailable() for a FIFO/pipe (measured on Qt 6.11 —
// a QFile on fd 0 sees zero bytes forever), so a QFile-based reader would never
// see a single host message and would never notice the host's EOF. These
// devices talk to the raw descriptors instead, with a QSocketNotifier for
// readiness — the same shape QProcess uses internally.

// stdin: host→app JSON lines. Reads with ::read() as the socket notifier
// reports data and emits readChannelFinished() at EOF, which is the protocol's
// "the player is gone, quit immediately" signal.
class PipeReadDevice : public QIODevice {
  Q_OBJECT

public:
  explicit PipeReadDevice(int readFd, QObject* parent = nullptr);

  [[nodiscard]] bool isSequential() const override { return true; }
  [[nodiscard]] qint64 bytesAvailable() const override;
  [[nodiscard]] bool atEnd() const override;

protected:
  qint64 readData(char* data, qint64 maxSize) override;
  qint64 writeData(const char*, qint64) override { return -1; }

private:
  void onActivated();

  int m_fd;
  QByteArray m_buffer;
  bool m_atEof = false;
  QSocketNotifier* m_notifier = nullptr;
};

// stdout: app→host JSON lines. Write-through so a line reaches the host as
// soon as it is written (stderr stays the app's log sink).
//
// Writes are bounded and never fatal: the descriptor is made non-blocking, and
// a write that cannot make progress within the write budget — or that fails —
// marks the device broken (one qWarning, then -1 with no further ::write).
// The app must stay responsive while the host is not draining: §2's quit
// trigger is stdin EOF, never a stalled or dead stdout. A full pipe means the
// host has a pipe's worth (~64 KiB) of undelivered lines queued behind it, so
// the budget is only ever hit by a host that has genuinely stopped reading.
//
// SIGPIPE (whose default action kills the process before ::write can return
// EPIPE) is ignored by the constructor — the same idiom Qt uses for QProcess
// and QAbstractSocket. main() sets the same process-wide policy at startup;
// the device does not rely on it, because the feed test drives this class with
// no main() of its own.
class PipeWriteDevice : public QIODevice {
  Q_OBJECT

public:
  explicit PipeWriteDevice(int writeFd, QObject* parent = nullptr);

  [[nodiscard]] bool isSequential() const override { return true; }

protected:
  qint64 readData(char*, qint64) override { return -1; }
  qint64 writeData(const char* data, qint64 maxSize) override;

private:
  int m_fd;
  // Latched by the first failed/given-up write: every later write is a silent
  // -1, so a broken sink cannot spam the log once per spectrum frame.
  bool m_broken = false;
};
