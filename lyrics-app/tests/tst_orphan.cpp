/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QTest>
#include <QtGlobal>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

// <sys/wait.h> includes <signal.h> on glibc, but POSIX does not require that
// transitive include; keep the direct one.
#include <csignal> // NOLINT(misc-include-cleaner): see the comment above.
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host/orphansweep.h"
#include "testbootstrap.h"

// Hermetic tests for src/host/orphansweep.{h,cpp}.
//
// Four decoys are forked with a real `lx-lyrics-app --player-feed` argv: two
// orphans (one ignores SIGTERM, so the sweep must escalate to SIGKILL), a
// process whose parent is this live test process, and an orphan without the
// feed flag. A sweep must kill exactly the first two and leave the others
// alone. The destructors kill every decoy on every path, including a QVERIFY
// early return.

namespace {

// Forces the offscreen platform (see testbootstrap.h).
static const bool s_offscreenBootstrap = kForceOffscreen;

constexpr const char* kPoliteScript = "while :; do sleep 0.2; done";
constexpr const char* kDeafScript = "trap '' TERM; while :; do sleep 0.2; done";
constexpr std::size_t kReadBufferSize = 8192;
constexpr std::size_t kPathSize = 64;

bool processAlive(pid_t pid)
{
  if (pid <= 0) {
    return false;
  }
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld/stat", static_cast<long>(pid));
  const int fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::array<char, 1024> buffer{};
  const ssize_t count = ::read(fd, buffer.data(), buffer.size() - 1);
  ::close(fd);
  if (count <= 0) {
    return false;
  }
  buffer[static_cast<std::size_t>(count)] = '\0';
  const char* close = std::strrchr(buffer.data(), ')');
  if (!close || close[1] != ' ' || close[2] == '\0') {
    return false;
  }
  const char state = close[2];
  return state != 'Z' && state != 'X' && state != 'x';
}

bool cmdlineHas(pid_t pid, const char* wanted)
{
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld/cmdline", static_cast<long>(pid));
  const int fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::array<char, kReadBufferSize> buffer{};
  const ssize_t count = ::read(fd, buffer.data(), buffer.size() - 1);
  ::close(fd);
  if (count <= 0) {
    return false;
  }
  buffer[static_cast<std::size_t>(count)] = '\0';
  const std::size_t length = static_cast<std::size_t>(count);
  std::size_t offset = 0;
  while (offset < length) {
    const char* argument = buffer.data() + offset;
    if (std::strcmp(argument, wanted) == 0) {
      return true;
    }
    offset += std::strlen(argument) + 1;
  }
  return false;
}

/// Double-forks and execs /bin/sh with argv[0] = "lx-lyrics-app", so the
/// grandchild is reparented away from this test (pid 1, or systemd --user on a
/// systemd desktop — that is what the sweep's subreaper clause exists for).
/// The grandchild's pid comes back through a pipe before it execs.
pid_t spawnOrphan(const char* script, bool with_feed_flag)
{
  std::array<int, 2> sync{};
  if (::pipe(sync.data()) != 0) {
    return -1;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(sync[0]);
    ::close(sync[1]);
    return -1;
  }
  if (child == 0) {
    ::close(sync[0]);
    if (::setsid() < 0) {
      _exit(127);
    }
    if (::fork() > 0) {
      _exit(0);
    }
    const pid_t self = ::getpid();
    // Best effort: the parent may already have gone away in a broken run.
    (void)::write(sync[1], &self, sizeof(self));
    ::close(sync[1]);
    if (with_feed_flag) {
      ::execl("/bin/sh", "lx-lyrics-app", "-c", script, "--player-feed",
              static_cast<char*>(nullptr));
    } else {
      ::execl("/bin/sh", "lx-lyrics-app", "-c", script, static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  ::close(sync[1]);
  pid_t grandchild = -1;
  const ssize_t count = ::read(sync[0], &grandchild, sizeof(grandchild));
  ::close(sync[0]);
  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  if (count != static_cast<ssize_t>(sizeof(grandchild))) {
    return -1;
  }
  return grandchild;
}

/// A direct child of the test process, so its parent is alive and is not the
/// systemd user manager: the sweep must never touch it.
pid_t spawnLiveChild(const char* script)
{
  const pid_t child = ::fork();
  if (child == 0) {
    ::execl("/bin/sh", "lx-lyrics-app", "-c", script, "--player-feed", static_cast<char*>(nullptr));
    _exit(127);
  }
  return child;
}

/// Kills and reaps a decoy on every path, including a failed QVERIFY return.
class Decoy {
public:
  explicit Decoy(pid_t pid = -1)
    : m_pid(pid)
  {
  }

  ~Decoy() { killNow(); }

  Decoy(const Decoy&) = delete;
  Decoy& operator=(const Decoy&) = delete;

  [[nodiscard]] pid_t pid() const { return m_pid; }

  void killNow()
  {
    if (m_pid <= 0) {
      return;
    }
    ::kill(m_pid, SIGCONT);
    ::kill(m_pid, SIGKILL);
    int status = 0;
    while (::waitpid(m_pid, &status, WNOHANG) < 0 && errno == EINTR) {
    }
    m_pid = -1;
  }

private:
  pid_t m_pid;
};

class TestOrphanSweep : public QObject {
  Q_OBJECT

private slots:
  void killsOnlyOrphans();
};

void TestOrphanSweep::killsOnlyOrphans()
{
  Decoy polite(spawnOrphan(kPoliteScript, true));
  QVERIFY(polite.pid() > 0);
  Decoy deaf(spawnOrphan(kDeafScript, true));
  QVERIFY(deaf.pid() > 0);
  Decoy live(spawnLiveChild(kPoliteScript));
  QVERIFY(live.pid() > 0);
  Decoy wrong(spawnOrphan(kPoliteScript, false));
  QVERIFY(wrong.pid() > 0);

  QTRY_VERIFY_WITH_TIMEOUT(cmdlineHas(polite.pid(), "--player-feed"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(cmdlineHas(deaf.pid(), "--player-feed"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(cmdlineHas(live.pid(), "--player-feed"), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(cmdlineHas(wrong.pid(), "-c"), 5000);

  QVERIFY(processAlive(polite.pid()));
  QVERIFY(processAlive(deaf.pid()));
  QVERIFY(processAlive(live.pid()));
  QVERIFY(processAlive(wrong.pid()));

  const int signalled = reapOrphanFeeds();
  QVERIFY(signalled >= 2);

  QTRY_VERIFY_WITH_TIMEOUT(!processAlive(polite.pid()), 5000);
  QTRY_VERIFY_WITH_TIMEOUT(!processAlive(deaf.pid()), 5000);
  QVERIFY(processAlive(live.pid()));
  QVERIFY(processAlive(wrong.pid()));
}

} // namespace

QTEST_GUILESS_MAIN(TestOrphanSweep)

#include "tst_orphan.moc"
