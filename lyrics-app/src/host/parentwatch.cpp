/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "host/parentwatch.h"

#include <QTimer>

#include <array>
#include <cstdio>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace {

/// /proc/<pid>/comm is TASK_COMM_LEN (16) bytes at most, newline included.
constexpr std::size_t kCommBufferSize = 32;

} // namespace

ParentWatch::ParentWatch(QObject* parent)
  : QObject(parent)
{
}

void ParentWatch::start(int intervalMs)
{
  if (m_started) {
    return;
  }
  m_started = true;
  m_parentPid = ::getppid();

  // A parent that died before main() left us with a subreaper (or init) as our
  // parent; there is no earlier pid to compare against, so recognise that
  // state directly. This is the only time /proc is read.
  if (initiallyOrphaned()) {
    // Defer the emission into the event loop: main() finishes wiring
    // `orphaned()` before exec(), and a quit() issued before exec() would be
    // swallowed (QCoreApplication::exec resets the quit flag).
    QTimer::singleShot(0, this, &ParentWatch::reportOrphaned);
    return;
  }

  m_timer = new QTimer(this);
  m_timer->setInterval(intervalMs);
  connect(m_timer, &QTimer::timeout, this, &ParentWatch::check);
  m_timer->start();
}

bool ParentWatch::initiallyOrphaned() const
{
  if (m_parentPid <= 1) {
    return true;
  }

  std::array<char, 64> path{};
  const int length =
    std::snprintf(path.data(), path.size(), "/proc/%ld/comm", static_cast<long>(m_parentPid));
  if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
    return false;
  }

  const int fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::array<char, kCommBufferSize> comm{};
  const ssize_t count = ::read(fd, comm.data(), comm.size() - 1);
  ::close(fd);
  if (count <= 0) {
    return false;
  }

  std::size_t end = static_cast<std::size_t>(count);
  while (end > 0 && (comm[end - 1] == '\n' || comm[end - 1] == '\r')) {
    --end;
  }
  return std::string_view(comm.data(), end) == "systemd";
}

void ParentWatch::check()
{
  // One syscall per tick: a living parent's pid cannot change, so any
  // difference means it died and we were reparented.
  if (::getppid() != m_parentPid) {
    reportOrphaned();
  }
}

void ParentWatch::reportOrphaned()
{
  if (m_reported) {
    return;
  }
  m_reported = true;
  if (m_timer) {
    m_timer->stop();
  }
  emit orphaned();
}
