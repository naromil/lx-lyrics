/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "host/orphansweep.h"

#include <QtGlobal>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#include <utility>
#include <vector>

#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

/// The one argument that marks a process as a lyrics feed child (feed.c runs
/// `<app_path> --player-feed`, and main.cpp only sweeps in that mode).
constexpr const char* kFeedFlag = "--player-feed";
/// The display app's basename, matched against argv[0] or /proc/<pid>/exe.
constexpr const char* kAppName = "lx-lyrics-app";
/// How long a candidate gets to honour SIGTERM before it is killed.
constexpr int kTermGraceMs = 1000;
/// Poll cadence while waiting for a signalled process to disappear.
constexpr int kPollStepMs = 25;
/// /proc/<pid>/cmdline read budget: argv is app_path + --player-feed + NULs,
/// and PATH_MAX bounds a path, so this cannot truncate a real feed child.
constexpr std::size_t kCmdlineSize = 8192;
/// Exe link budget (readlink does not NUL-terminate; one byte is kept free).
constexpr std::size_t kExeSize = 4096;
/// Enough for "/proc/" + a pid + "/..." and its terminator.
constexpr std::size_t kPathSize = 64;

bool isNumeric(const char* text)
{
  if (!text || *text == '\0') {
    return false;
  }
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
  }
  return true;
}

/// Reads at most @p size - 1 bytes of a /proc file and NUL-terminates it. A
/// missing or unreadable entry is a normal race and reports false.
bool readProcFile(const char* path, char* buffer, std::size_t size, std::size_t* length)
{
  if (size == 0) {
    return false;
  }
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const ssize_t count = ::read(fd, buffer, size - 1);
  ::close(fd);
  if (count < 0) {
    return false;
  }
  buffer[count] = '\0';
  if (length) {
    *length = static_cast<std::size_t>(count);
  }
  return true;
}

const char* baseName(const char* path)
{
  const char* slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

bool sameRealUid(pid_t pid)
{
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld", static_cast<long>(pid));
  struct stat info{};
  if (::stat(path.data(), &info) != 0) {
    return false;
  }
  return info.st_uid == ::getuid();
}

/// Reads `PPid:` from /proc/<pid>/status.
bool readParentPid(pid_t pid, pid_t* parent)
{
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld/status", static_cast<long>(pid));
  std::array<char, 4096> buffer{};
  std::size_t length = 0;
  if (!readProcFile(path.data(), buffer.data(), buffer.size(), &length)) {
    return false;
  }
  const char* line = buffer.data();
  const char* const buffer_end = buffer.data() + length;
  while (line < buffer_end) {
    const char* end = std::strchr(line, '\n');
    const std::size_t line_length = end ? static_cast<std::size_t>(end - line) : std::strlen(line);
    if (line_length > 5 && std::strncmp(line, "PPid:", 5) == 0) {
      errno = 0;
      char* number_end = nullptr;
      const long value = std::strtol(line + 5, &number_end, 10);
      if (number_end == line + 5 || errno == ERANGE || value <= 0) {
        return false;
      }
      *parent = static_cast<pid_t>(value);
      return true;
    }
    if (!end) {
      break;
    }
    line = end + 1;
  }
  return false;
}

/// Reads the state letter from /proc/<pid>/stat. The comm field is wrapped in
/// parentheses and may itself contain spaces or parentheses, so the state is
/// the field after the LAST ')' (proc(5)).
bool readProcessState(pid_t pid, char* state)
{
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld/stat", static_cast<long>(pid));
  std::array<char, 1024> buffer{};
  if (!readProcFile(path.data(), buffer.data(), buffer.size(), nullptr)) {
    return false;
  }
  const char* close = std::strrchr(buffer.data(), ')');
  if (!close || close[1] != ' ' || close[2] == '\0') {
    return false;
  }
  *state = close[2];
  return true;
}

/// A zombie has no process left to signal (and is about to be reaped), so it
/// counts as gone for the wait loop below.
bool processAlive(pid_t pid)
{
  char state = '\0';
  if (!readProcessState(pid, &state)) {
    return false;
  }
  return state != 'Z' && state != 'X' && state != 'x';
}

bool parentIsSystemd(pid_t parent)
{
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld/comm", static_cast<long>(parent));
  std::array<char, 64> buffer{};
  std::size_t length = 0;
  if (!readProcFile(path.data(), buffer.data(), buffer.size(), &length)) {
    return false;
  }
  if (length > 0 && buffer[length - 1] == '\n') {
    buffer[length - 1] = '\0';
  }
  return std::strcmp(buffer.data(), "systemd") == 0;
}

/// The process is an orphan: reparented (pid 1 or the systemd user manager,
/// which is a child subreaper) or its recorded parent is gone / a zombie.
bool isOrphan(pid_t pid, pid_t* parent_out)
{
  pid_t parent = 0;
  if (!readParentPid(pid, &parent) || parent <= 0) {
    return false;
  }
  *parent_out = parent;
  if (parent == 1) {
    return true;
  }
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld", static_cast<long>(parent));
  struct stat info{};
  if (::stat(path.data(), &info) != 0) {
    // Only a missing parent directory means "gone"; an unreadable one is not
    // evidence of an orphan, so leave that process alone.
    return errno == ENOENT;
  }
  char state = '\0';
  if (readProcessState(parent, &state) && (state == 'Z' || state == 'X' || state == 'x')) {
    return true;
  }
  return parentIsSystemd(parent);
}

/// `--player-feed` must be a whole argv entry, never a substring of a path.
bool hasFeedFlag(const char* cmdline, std::size_t length)
{
  std::size_t offset = 0;
  while (offset < length) {
    const char* argument = cmdline + offset;
    const std::size_t argument_length = std::strlen(argument);
    if (argument_length == std::strlen(kFeedFlag) && std::strcmp(argument, kFeedFlag) == 0) {
      return true;
    }
    offset += argument_length + 1;
  }
  return false;
}

/// argv[0] basename OR /proc/<pid>/exe basename is the display app. The exe
/// path covers a process whose argv[0] was overwritten; argv[0] covers the
/// gate's shell-based decoys (their exe is /bin/sh).
bool isAppProcess(pid_t pid, const char* cmdline)
{
  if (std::strcmp(baseName(cmdline), kAppName) == 0) {
    return true;
  }
  std::array<char, kPathSize> path{};
  std::snprintf(path.data(), path.size(), "/proc/%ld/exe", static_cast<long>(pid));
  std::array<char, kExeSize> exe{};
  const ssize_t count = ::readlink(path.data(), exe.data(), exe.size() - 1);
  if (count <= 0) {
    return false;
  }
  exe[static_cast<std::size_t>(count)] = '\0';
  // A replaced or deleted binary still reads `<path> (deleted)`.
  static constexpr std::string_view kDeletedSuffix = " (deleted)";
  const std::size_t exe_length = static_cast<std::size_t>(count);
  if (exe_length > kDeletedSuffix.size() &&
      std::string_view(exe.data() + exe_length - kDeletedSuffix.size()) == kDeletedSuffix) {
    exe[exe_length - kDeletedSuffix.size()] = '\0';
  }
  return std::strcmp(baseName(exe.data()), kAppName) == 0;
}

bool waitForExit(pid_t pid, int timeout_ms)
{
  int waited = 0;
  while (waited <= timeout_ms) {
    if (!processAlive(pid)) {
      return true;
    }
    const struct timespec step = {0, static_cast<long>(kPollStepMs) * 1000L * 1000L};
    ::nanosleep(&step, nullptr);
    waited += kPollStepMs;
  }
  return !processAlive(pid);
}

} // namespace

int reapOrphanFeeds()
{
  DIR* proc = ::opendir("/proc");
  if (!proc) {
    return 0; // No procfs (non-Linux): nothing to scan, nothing to fail.
  }

  const pid_t self = ::getpid();
  const pid_t own_parent = ::getppid();
  std::vector<std::pair<pid_t, pid_t>> victims;

  while (const struct dirent* entry = ::readdir(proc)) {
    if (!isNumeric(entry->d_name)) {
      continue;
    }
    const pid_t pid = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
    if (pid <= 1 || pid == self || pid == own_parent) {
      continue;
    }
    if (!sameRealUid(pid)) {
      continue;
    }

    std::array<char, kPathSize> path{};
    std::snprintf(path.data(), path.size(), "/proc/%ld/cmdline", static_cast<long>(pid));
    std::array<char, kCmdlineSize> cmdline{};
    std::size_t length = 0;
    if (!readProcFile(path.data(), cmdline.data(), cmdline.size(), &length) || length == 0) {
      continue;
    }
    if (!hasFeedFlag(cmdline.data(), length) || !isAppProcess(pid, cmdline.data())) {
      continue;
    }

    pid_t parent = 0;
    if (!isOrphan(pid, &parent)) {
      continue;
    }
    victims.emplace_back(pid, parent);
  }
  ::closedir(proc);

  int signalled = 0;
  for (const auto& [pid, parent] : victims) {
    if (::kill(pid, SIGTERM) != 0) {
      if (errno == EPERM) {
        qWarning("cannot signal orphaned lyrics feed pid=%lld (ppid=%lld): %s",
                 static_cast<long long>(pid), static_cast<long long>(parent), std::strerror(errno));
      }
      continue; // ESRCH (gone between scan and kill) and EPERM are both non-fatal.
    }
    qWarning("reaped orphaned lyrics feed pid=%lld (ppid=%lld)", static_cast<long long>(pid),
             static_cast<long long>(parent));
    ++signalled;

    if (!waitForExit(pid, kTermGraceMs)) {
      if (::kill(pid, SIGKILL) != 0) {
        if (errno != ESRCH) {
          qWarning("cannot kill orphaned lyrics feed pid=%lld: %s", static_cast<long long>(pid),
                   std::strerror(errno));
        }
      } else {
        waitForExit(pid, kTermGraceMs);
      }
    }

    // Never block: only a child of ours can be reaped here, and an orphan
    // candidate is not (its parent is elsewhere). WNOHANG turns that into a
    // harmless ECHILD for every other process.
    int status = 0;
    ::waitpid(pid, &status, WNOHANG);
  }
  return signalled;
}
