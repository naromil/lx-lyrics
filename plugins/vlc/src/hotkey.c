/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * The global-shortcut transport (see hotkey.h): the plugin side of the
 * lx-lyrics-hotkey helper.
 *
 * The helper is spawned with the write end of a pipe on fd 3 and reports back
 * one line at a time: "toggle" for each press, and "ready <trigger>" or
 * "unavailable <reason>" once for the portal binding. The pipe is the only
 * channel; the helper's stdout/stderr are diagnostics.
 *
 * Spawning, reading and reaping are plain POSIX calls, so this file stays free
 * of GLib and Audacious headers and is hermetically testable.
 */

#include "hotkey.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/** Longest helper line kept; a longer line is dropped rather than fragmented. */
#define LX_HOTKEY_LINE_MAX 4096
/** poll() slice in the reader loop; bounds how long stop() waits for it. */
#define LX_HOTKEY_POLL_MS 50
/** How long the helper gets to honour SIGTERM before SIGKILL. */
#define LX_HOTKEY_TERM_GRACE_MS 1000
/** The child reads and writes from the helper on this descriptor. */
#define LX_HOTKEY_PIPE_FD 3

struct lx_hotkey {
  pid_t pid;
  int read_fd;
  pthread_t thread;
  bool thread_started;
  /* Set by stop() and read by the reader, so it wakes out of poll() promptly. */
  atomic_bool stopping;
  /* stop() is documented as idempotent, so the handle is never freed: the
   * dictated API has no separate destroy call. One allocation per start(). */
  bool stopped;
  lx_hotkey_toggle_fn on_toggle;
  lx_hotkey_status_fn on_status;
  void* userdata;
};

static void dispatch_line(lx_hotkey_t* hotkey, const char* line)
{
  if (strcmp(line, "toggle") == 0) {
    if (hotkey->on_toggle) {
      hotkey->on_toggle(hotkey->userdata);
    }
    return;
  }
  if (hotkey->on_status) {
    hotkey->on_status(line, hotkey->userdata);
  }
}

/* The reader thread: split the pipe into newline-terminated lines and deliver
 * each one. EOF (the helper exited) ends the loop; there is nothing to spin on. */
static void* reader_main(void* arg)
{
  lx_hotkey_t* hotkey = arg;
  char line[LX_HOTKEY_LINE_MAX];
  size_t used = 0;
  bool discarding = false;

  while (!atomic_load(&hotkey->stopping)) {
    struct pollfd pfd = {.fd = hotkey->read_fd, .events = POLLIN};
    const int ready = poll(&pfd, 1, LX_HOTKEY_POLL_MS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      continue;
    }

    char chunk[512];
    const ssize_t got = read(hotkey->read_fd, chunk, sizeof(chunk));
    if (got < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      break;
    }
    if (got == 0) {
      break;
    }

    for (ssize_t i = 0; i < got; i++) {
      const char byte = chunk[i];
      if (byte == '\n') {
        if (!discarding) {
          line[used] = '\0';
          dispatch_line(hotkey, line);
        }
        used = 0;
        discarding = false;
      } else if (!discarding) {
        if (used + 1 < sizeof(line)) {
          line[used++] = byte;
        } else {
          /* An overlong line is not a helper report; drop it whole. */
          discarding = true;
          used = 0;
        }
      }
    }
  }
  return NULL;
}

/* Reaps the child after the SIGTERM/SIGKILL escalation. Never leaves a zombie.
 * The helper owns a process group (see lx_hotkey_start()), so the stop path
 * signals that group: a helper that spawned children of its own cannot leave
 * them behind holding the plugin's pipes. */
static void reap_helper(pid_t pid)
{
  int status = 0;
  for (int waited = 0; waited < LX_HOTKEY_TERM_GRACE_MS; waited += 10) {
    const pid_t reaped = waitpid(pid, &status, WNOHANG);
    if (reaped == pid) {
      return;
    }
    if (reaped < 0 && errno != EINTR) {
      return;
    }
    usleep(10 * 1000);
  }
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}

lx_hotkey_t* lx_hotkey_start(const lx_hotkey_config_t* config)
{
  if (!config || !config->helper_path || config->helper_path[0] == '\0') {
    return NULL;
  }
  if (access(config->helper_path, X_OK) != 0) {
    return NULL;
  }

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    return NULL;
  }
  /* The read end belongs to this process only; the child gets a duplicate of
   * the write end as fd 3 and closes its inherited copy. */
  fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);

  const char* argv[6];
  int argc = 0;
  argv[argc++] = config->helper_path;
  argv[argc++] = "--pipe-fd";
  argv[argc++] = "3";
  if (config->accelerator && config->accelerator[0] != '\0') {
    argv[argc++] = "--accelerator";
    argv[argc++] = config->accelerator;
  }
  argv[argc] = NULL;

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return NULL;
  }
  if (pid == 0) {
    /* Child: only async-signal-safe calls until execv(). fd 3 must be the
     * pipe write end and must survive exec. Close our read end first: it may
     * itself be fd 3, in which case closing it after dup2() would close the
     * duplicate. */
    setpgid(0, 0);
    if (pipe_fds[0] != LX_HOTKEY_PIPE_FD) {
      close(pipe_fds[0]);
    }
    if (pipe_fds[1] == LX_HOTKEY_PIPE_FD) {
      fcntl(LX_HOTKEY_PIPE_FD, F_SETFD, 0);
    } else {
      if (dup2(pipe_fds[1], LX_HOTKEY_PIPE_FD) < 0) {
        _exit(127);
      }
      close(pipe_fds[1]);
    }
    execv(config->helper_path, (char* const*)argv);
    _exit(127);
  }

  /* Also from the parent, so the group exists even if stop() races the
   * child's own setpgid(); EACCES after exec is harmless. */
  setpgid(pid, pid);
  close(pipe_fds[1]);

  lx_hotkey_t* hotkey = calloc(1, sizeof(*hotkey));
  if (!hotkey) {
    close(pipe_fds[0]);
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return NULL;
  }
  hotkey->pid = pid;
  hotkey->read_fd = pipe_fds[0];
  hotkey->on_toggle = config->on_toggle;
  hotkey->on_status = config->on_status;
  hotkey->userdata = config->userdata;
  atomic_init(&hotkey->stopping, false);

  if (pthread_create(&hotkey->thread, NULL, reader_main, hotkey) != 0) {
    close(hotkey->read_fd);
    kill(-hotkey->pid, SIGTERM);
    kill(hotkey->pid, SIGTERM);
    while (waitpid(hotkey->pid, NULL, 0) < 0 && errno == EINTR) {
    }
    free(hotkey);
    return NULL;
  }
  hotkey->thread_started = true;
  return hotkey;
}

void lx_hotkey_stop(lx_hotkey_t* hotkey)
{
  if (!hotkey || hotkey->stopped) {
    return;
  }
  hotkey->stopped = true;

  atomic_store(&hotkey->stopping, true);
  if (hotkey->thread_started) {
    pthread_join(hotkey->thread, NULL);
    hotkey->thread_started = false;
  }
  if (hotkey->read_fd >= 0) {
    close(hotkey->read_fd);
    hotkey->read_fd = -1;
  }
  if (hotkey->pid > 0) {
    /* The helper's group (see lx_hotkey_start()): a shell or a wrapper that
     * spawned a child of its own must not outlive the session. */
    kill(-hotkey->pid, SIGTERM);
    kill(hotkey->pid, SIGTERM);
    reap_helper(hotkey->pid);
    hotkey->pid = 0;
  }
}
