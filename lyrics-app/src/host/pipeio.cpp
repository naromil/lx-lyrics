/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "host/pipeio.h"

#include <QDebug>
#include <QSocketNotifier>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace {

/// One ::read() per notifier activation: the notifier is level-triggered, so
/// pending bytes re-arm it immediately, while a single read cannot block even
/// though the descriptor is in blocking mode.
constexpr std::size_t kReadChunk = 64 * 1024;

/// How long one app→host write may wait for the host to drain its stdout
/// before the device drops the line. A pipe holds ~64 KiB, so hitting EAGAIN
/// already means ~50 s of request lines are queued behind an unheard host; the
/// budget only bounds how long the app may sit there (it must keep servicing
/// stdin, whose EOF is §2's quit trigger) before it gives up on that sink.
constexpr int kWriteTimeoutMs = 250;

/// SIGPIPE's default action terminates the process, and it is raised by a
/// ::write() to a pipe whose read end is gone BEFORE the call can return
/// EPIPE — so an ignored SIGPIPE is what lets the write path report the
/// failure instead of dying. Ignore it once, process-wide; the same idiom Qt
/// uses internally for QProcess/QAbstractSocket. main() also sets this at
/// startup, but this device must not depend on main() (the feed test drives it
/// directly).
void ignoreSigpipe()
{
  static std::once_flag once;
  std::call_once(once, [] {
    ::signal(SIGPIPE, SIG_IGN);
  });
}

} // namespace

PipeReadDevice::PipeReadDevice(int readFd, QObject* parent)
  : QIODevice(parent)
  , m_fd(readFd)
{
  open(QIODevice::ReadOnly);

  m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
  connect(m_notifier, &QSocketNotifier::activated, this, &PipeReadDevice::onActivated);
}

qint64 PipeReadDevice::bytesAvailable() const
{
  return m_buffer.size() + QIODevice::bytesAvailable();
}

bool PipeReadDevice::atEnd() const
{
  return m_atEof && m_buffer.isEmpty();
}

qint64 PipeReadDevice::readData(char* data, qint64 maxSize)
{
  if (m_buffer.isEmpty())
    return 0; // Nothing buffered; onActivated() reports new data via readyRead.

  const qint64 count = std::min<qint64>(maxSize, m_buffer.size());
  std::memcpy(data, m_buffer.constData(), static_cast<size_t>(count));
  m_buffer.remove(0, count);
  return count;
}

void PipeReadDevice::onActivated()
{
  std::array<char, kReadChunk> chunk{};
  ssize_t count = ::read(m_fd, chunk.data(), chunk.size());

  if (count < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return; // Try again on the next activation.
    }
    qWarning("pipeio: read failed (%s); treating the pipe as closed", std::strerror(errno));
    count = 0;
  }

  if (count == 0) {
    // The parent closed the pipe (§2): no more messages will ever arrive, and
    // the notifier would spin on a permanently readable descriptor.
    m_notifier->setEnabled(false);
    m_atEof = true;
    emit readChannelFinished();
    return;
  }

  m_buffer.append(chunk.data(), static_cast<qsizetype>(count));
  emit readyRead();
}

PipeWriteDevice::PipeWriteDevice(int writeFd, QObject* parent)
  : QIODevice(parent)
  , m_fd(writeFd)
{
  ignoreSigpipe();

  // Non-blocking: a host that stops draining its stdout must never park the
  // event-loop thread inside ::write (see the header comment).
  const int flags = ::fcntl(m_fd, F_GETFL);
  if (flags < 0 || ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    qWarning("pipeio: cannot make fd %d non-blocking (%s); writes may block when the host stalls",
             m_fd, std::strerror(errno));
  }

  open(QIODevice::WriteOnly);
}

qint64 PipeWriteDevice::writeData(const char* data, qint64 maxSize)
{
  if (m_broken) {
    return -1; // Already reported: never touch a dead sink again.
  }

  const auto giveUp = [this](const char* why) {
    m_broken = true;
    qWarning("pipeio: cannot write to the host (%s); dropping app→host lines from here on", why);
  };

  qint64 written = 0;
  while (written < maxSize) {
    const ssize_t count = ::write(m_fd, data + written, static_cast<size_t>(maxSize - written));
    if (count > 0) {
      written += count;
      continue;
    }
    if (count == 0) {
      break; // A pipe reports either a short write or an error, never 0.
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      giveUp(std::strerror(errno)); // EPIPE: the host's reader is gone.
      return -1;
    }

    // The pipe is full: the host is not reading its stdout. Wait a bounded
    // moment for room, then drop the line — the app keeps running, and §2's
    // stdin EOF still ends it (quitting here would be a different trigger).
    pollfd writable{m_fd, POLLOUT, 0};
    const int ready = ::poll(&writable, 1, kWriteTimeoutMs);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready <= 0 || (writable.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      giveUp(ready == 0 ? "stdout not drained within the write budget" : "stdout is unusable");
      return -1;
    }
  }
  return written;
}
