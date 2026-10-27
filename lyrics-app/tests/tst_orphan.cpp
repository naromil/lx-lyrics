/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QFileInfo>
#include <QProcess>
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

// Hermetic tests for src/host/orphansweep.{h,cpp} plus one end-to-end
// regression test for the app's in-process parent watch.
//
// Four decoys are forked with a real `lx-lyrics-app --player-feed` argv: two
// orphans (one ignores SIGTERM, so the sweep must escalate to SIGKILL), a
// process whose parent is this live test process, and an orphan without the
// feed flag. A sweep must kill exactly the first two and leave the others
// alone. The destructors kill every decoy on every path, including a QVERIFY
// early return.
//
// The last slot runs the real `lx-lyrics-app --player-feed` under a wrapper
// shell whose stdin write end the test keeps open, then SIGKILLs the shell:
// only the app's own parent watch can end it, since no EOF is possible.
//
// The start-time slot covers the pid-reuse guard: a live pid reads a stable
// kernel start time, a never-allocated pid is unreadable, and a deliberately
// wrong start time is rejected — the comparison the sweep makes before each
// signal, so a reused pid can never be hit.

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

/// Single-quotes a path for /bin/sh, escaping embedded single quotes.
QString shellQuoted(const QString& value)
{
  QString quoted = value;
  quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

/// Reads the wrapper's stdout without blocking and returns the first complete
/// numeric line (the shell's `echo $!`). Partial lines stay buffered.
pid_t takeWrapperPid(QProcess& wrapper, QByteArray& buffer)
{
  buffer += wrapper.readAllStandardOutput();
  const qsizetype newline = buffer.lastIndexOf('\n');
  if (newline < 0) {
    return -1;
  }
  const QList<QByteArray> lines = buffer.left(newline).split('\n');
  for (const QByteArray& line : lines) {
    bool ok = false;
    const qlonglong value = line.trimmed().toLongLong(&ok);
    if (ok && value > 1) {
      return static_cast<pid_t>(value);
    }
  }
  return -1;
}

/// Independently extracts the starttime field (overall field 22) from
/// /proc/<pid>/stat: everything after the LAST ')' is split by QByteArray,
/// where token 0 is the state, so starttime is token 19. A different parser
/// from the production loop, so a shifted index cannot cancel out. Empty on
/// any failure.
QByteArray statStarttimeToken(pid_t pid)
{
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld/stat", static_cast<long>(pid));
  const int fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {};
  }
  std::array<char, 1024> buffer{};
  const ssize_t count = ::read(fd, buffer.data(), buffer.size());
  ::close(fd);
  if (count <= 0) {
    return {};
  }
  const QByteArray stat(buffer.data(), static_cast<qsizetype>(count));
  const qsizetype close = stat.lastIndexOf(')');
  if (close < 0) {
    return {};
  }
  const QList<QByteArray> fields = stat.mid(close + 1).trimmed().split(' ');
  constexpr int kStarttimeIndex = 19;
  return kStarttimeIndex < fields.size() ? fields.at(kStarttimeIndex) : QByteArray();
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
  void startTimeIdentity();
  void quitsWhenParentDies();
  void refusesSessionRestore();
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

// Regression: a pid is not an identity. The sweep records each candidate's
// kernel start time during the scan and re-checks it before every signal, so
// a victim that exits and whose pid is reused cannot make a deferred SIGTERM
// or SIGKILL hit an unrelated process. These are the exact helpers the signal
// path calls.
void TestOrphanSweep::startTimeIdentity()
{
  quint64 starttime = 0;
  QVERIFY(readProcessStartTime(::getpid(), &starttime));

  // The field index is load-bearing: after the last ')' token 0 is the state
  // (field 3), so starttime (field 22) must be token 19.
  bool parsed = false;
  const qulonglong expected = statStarttimeToken(::getpid()).toULongLong(&parsed);
  QVERIFY(parsed);
  QCOMPARE(starttime, static_cast<quint64>(expected));

  // Stable: a second read of the same live process agrees.
  quint64 again = 0;
  QVERIFY(readProcessStartTime(::getpid(), &again));
  QCOMPARE(again, starttime);
  QVERIFY(processStartTimeMatches(::getpid(), starttime));

  // Linux never allocates PID_MAX_LIMIT (4194304) or above, so the next value
  // has no /proc entry ever: unreadable means "gone", never a match.
  constexpr qint64 kNeverAllocatedPid = 4194305;
  quint64 missing = 0;
  QVERIFY(!readProcessStartTime(kNeverAllocatedPid, &missing));
  QVERIFY(!processStartTimeMatches(kNeverAllocatedPid, starttime));

  // A wrong start time means the pid now belongs to another process: the
  // comparison must reject it so the sweep leaves that process alone.
  QVERIFY(!processStartTimeMatches(::getpid(), starttime + 1));
  QVERIFY(!processStartTimeMatches(::getpid(), starttime == 0 ? 1 : starttime - 1));
}

// Regression: a feed child whose parent dies WITHOUT closing its stdin must
// end itself. Only the app's ParentWatch can do that — the wrapper's stdin
// write channel is never closed here, so no EOF can arrive.
void TestOrphanSweep::quitsWhenParentDies()
{
  const QFileInfo app(QCoreApplication::applicationDirPath() + QStringLiteral("/lx-lyrics-app"));
  QVERIFY2(app.isExecutable(), qPrintable(app.absoluteFilePath()));

  QProcess wrapper;
  // The app's stderr goes to ours, never into an undrained pipe.
  wrapper.setProcessChannelMode(QProcess::ForwardedErrorChannel);

  // QProcess closes its OWN stdin channel when the observed child (the shell)
  // dies, which would hand the app a genuine EOF and mask the self-watch. Give
  // the wrapper the read end of a pipe this test owns instead, and keep the
  // write end open for the whole test (closeWriteChannel() is never called):
  // the app can then only quit through its parent watch.
  std::array<int, 2> inputPipe{};
  QVERIFY(::pipe(inputPipe.data()) == 0);
  const int inputReadFd = inputPipe[0];
  const int inputWriteFd = inputPipe[1];
  wrapper.setStandardInputFile(QStringLiteral("/dev/fd/%1").arg(inputReadFd));

  wrapper.start(QStringLiteral("/bin/sh"),
                {QStringLiteral("-c"), shellQuoted(app.absoluteFilePath()) +
                                         QStringLiteral(" --player-feed <&0 & echo $!; wait")});
  QVERIFY(wrapper.waitForStarted(5000));
  ::close(inputReadFd); // The shell holds its own copy of the read end.

  pid_t appPid = -1;
  QByteArray output;
  const bool havePid = QTest::qWaitFor(
    [&appPid, &wrapper, &output] {
      appPid = takeWrapperPid(wrapper, output);
      return appPid > 0;
    },
    5000);
  QVERIFY(havePid);
  Decoy appGuard(appPid);

  QTRY_VERIFY_WITH_TIMEOUT(cmdlineHas(appGuard.pid(), "--player-feed"), 5000);
  QVERIFY(processAlive(appGuard.pid()));

  // SIGKILL the app's parent (the shell). The app is reparented; the stdin
  // write end in this process is still open, so only the self-watch notices
  // and quits — without a close_requested line.
  wrapper.kill();
  QVERIFY(wrapper.waitForFinished(5000));

  const bool gone = QTest::qWaitFor(
    [&appGuard] {
      return !processAlive(appGuard.pid());
    },
    8000);
  ::close(inputWriteFd);
  QVERIFY(gone);
}

// Regression: a window still open at logout came back after a reboot, because
// the desktop session manager restarted the app from its saved command line
// (argv + `-session <id>`) with no player behind it. Its parent (the session
// manager) stays alive and its stdin need not reach EOF, so neither quit
// trigger fires: the restored instance must refuse to run on its own.
void TestOrphanSweep::refusesSessionRestore()
{
  const QFileInfo app(QCoreApplication::applicationDirPath() + QStringLiteral("/lx-lyrics-app"));
  QVERIFY2(app.isExecutable(), qPrintable(app.absoluteFilePath()));

  QProcess restored;
  restored.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  // Live parent (this test) and an open stdin write channel (never closed
  // before the wait): only the restore check can end this process.
  restored.start(app.absoluteFilePath(),
                 {QStringLiteral("--player-feed"), QStringLiteral("-session"),
                  QStringLiteral("10d0c0ffee0000000000000000000000_1_0")});
  QVERIFY(restored.waitForStarted(5000));
  const bool finished = restored.waitForFinished(5000);
  if (!finished) {
    restored.kill();
    restored.waitForFinished(5000);
  }
  QVERIFY(finished);
  QCOMPARE(restored.exitStatus(), QProcess::NormalExit);
  QCOMPARE(restored.exitCode(), 0);
}

} // namespace

QTEST_GUILESS_MAIN(TestOrphanSweep)

#include "tst_orphan.moc"
