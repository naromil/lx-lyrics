/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 */

#define _GNU_SOURCE /* pipe2() */

#include "feed.h"

#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long lx_feed_stop() lets the reader thread wait for the app to quit after
 * its stdin was closed before it gives up on it. */
#define LX_FEED_STOP_GRACE_MS 2500
/* How long one line may wait for the app to drain its stdin. */
#define LX_FEED_WRITE_WAIT_MS 1000
#define LX_FEED_WRITE_STEP_MS 50
/* How long the child gets to leave on its own after the session ended, and how
 * long it gets to leave after SIGTERM before it is killed outright. The second
 * step is what keeps a wedged app from hanging the player: lx_feed_stop() runs
 * on the adapter's session thread, which VLC's Close() joins, so an unbounded
 * wait here would be an unbounded VLC shutdown. */
#define LX_FEED_REAP_WAIT_MS 1000
#define LX_FEED_KILL_WAIT_MS 1000

struct lx_feed {
  pid_t pid;
  int to_child;   /* write end of the app's stdin */
  int from_child; /* read end of the app's stdout */
  pthread_mutex_t write_lock;
  /* Guards refs/torn_down: a thread may hold a reference while another one ends
   * the session (see the threading contract in feed.h). */
  pthread_mutex_t ref_lock;
  int refs;
  bool torn_down;
  pthread_t thread;
  bool thread_started;
  /* Written by lx_feed_spawn()/lx_feed_stop()/the writers, read by the reader
   * thread: volatile so the loop re-reads it instead of caching it. */
  volatile sig_atomic_t stop_requested;
  bool stdin_open; /* guarded by write_lock */
  bool child_exited;
  /* Set by the reader thread when the app violated the protocol (§7); the
   * reason is passed to the protocol_error callback. */
  char protocol_error_reason[160];
  lx_feed_callbacks_t callbacks;
  void* userdata;
};

static void lx_log(const char* format, ...)
{
  va_list args;
  fputs("lxlyrics: ", stderr);
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

static int64_t lx_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * A growable line buffer. The JSON lives in the heap rather than in a fixed
 * array so a long path or tag can never be truncated into an invalid line, and
 * so the exact bytes written are the exact bytes built.
 */
typedef struct {
  char* data;
  size_t len;
  size_t cap;
  bool oom;
} lx_buf_t;

static void buf_reserve(lx_buf_t* buf, size_t extra)
{
  if (buf->oom || buf->cap - buf->len > extra) {
    return;
  }
  size_t cap = buf->cap ? buf->cap : 256;
  while (cap - buf->len <= extra) {
    cap *= 2;
  }
  char* data = realloc(buf->data, cap);
  if (!data) {
    buf->oom = true;
    return;
  }
  buf->data = data;
  buf->cap = cap;
}

static void buf_put_len(lx_buf_t* buf, const char* text, size_t length)
{
  buf_reserve(buf, length);
  if (buf->oom) {
    return;
  }
  memcpy(buf->data + buf->len, text, length);
  buf->len += length;
  buf->data[buf->len] = '\0';
}

static void buf_put(lx_buf_t* buf, const char* text)
{
  buf_put_len(buf, text, strlen(text));
}

static void buf_printf(lx_buf_t* buf, const char* format, ...)
{
  va_list args;
  char stack[64];
  va_start(args, format);
  int size = vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  if (size < 0) {
    return;
  }
  if ((size_t)size < sizeof(stack)) {
    buf_put_len(buf, stack, (size_t)size);
    return;
  }
  buf_reserve(buf, (size_t)size);
  if (buf->oom) {
    return;
  }
  va_start(args, format);
  vsnprintf(buf->data + buf->len, buf->cap - buf->len, format, args);
  va_end(args);
  buf->len += (size_t)size;
}

/* Appends @p text as the body of a JSON string (quotes are the caller's). */
static void buf_put_escaped(lx_buf_t* buf, const char* text)
{
  buf_reserve(buf, lx_json_escaped_size(text));
  if (buf->oom) {
    return;
  }
  buf->len += lx_json_escape(text, buf->data + buf->len, buf->cap - buf->len);
}

static void buf_release(lx_buf_t* buf)
{
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

/*
 * write() to a pipe whose reader is gone raises SIGPIPE, which would kill VLC.
 * Block it around the call and swallow the signal we generated instead of
 * changing the process-wide disposition from inside a plugin.
 */
static ssize_t lx_write_no_sigpipe(int fd, const void* data, size_t size)
{
  sigset_t block;
  sigset_t previous;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &block, &previous);
  ssize_t written = write(fd, data, size);
  if (written < 0 && errno == EPIPE) {
    sigset_t pending;
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
      struct timespec zero = {0, 0};
      sigtimedwait(&block, NULL, &zero);
    }
  }
  pthread_sigmask(SIG_SETMASK, &previous, NULL);
  return written;
}

/* Writes @p size bytes, waiting a bounded time for the app to drain the pipe
 * (the write end is non-blocking, so an app that stops reading can never wedge
 * the caller — the feed's reader thread in this adapter). Returns false when the
 * app is gone or too slow. */
static bool lx_write_all(struct lx_feed* feed, const char* data, size_t size)
{
  size_t offset = 0;
  int waited = 0;
  while (offset < size) {
    ssize_t written = lx_write_no_sigpipe(feed->to_child, data + offset, size - offset);
    if (written > 0) {
      offset += (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (waited >= LX_FEED_WRITE_WAIT_MS) {
        errno = EAGAIN;
        return false;
      }
      struct pollfd out = {.fd = feed->to_child, .events = POLLOUT, .revents = 0};
      if (poll(&out, 1, LX_FEED_WRITE_STEP_MS) < 0 && errno != EINTR) {
        return false;
      }
      waited += LX_FEED_WRITE_STEP_MS;
      continue;
    }
    return false;
  }
  return true;
}

/* Closes the app's stdin exactly once — protocol.md §2: EOF on stdin is how the
 * app is told to quit, so the write end must not stay open after the session
 * gives up on writing. Callers must hold write_lock. */
static void feed_close_stdin_locked(struct lx_feed* feed)
{
  if (feed->stdin_open) {
    close(feed->to_child);
    feed->to_child = -1;
    feed->stdin_open = false;
  }
}

static void feed_close_stdin(struct lx_feed* feed)
{
  pthread_mutex_lock(&feed->write_lock);
  feed_close_stdin_locked(feed);
  pthread_mutex_unlock(&feed->write_lock);
}

/* Serializes one host->app line onto the app's stdin. */
static void feed_emit(struct lx_feed* feed, lx_buf_t* line, const char* what)
{
  if (line->oom) {
    lx_log("out of memory: dropped %s", what);
    buf_release(line);
    return;
  }
  pthread_mutex_lock(&feed->write_lock);
  if (feed->stdin_open) {
    if (!lx_write_all(feed, line->data, line->len) || !lx_write_all(feed, "\n", 1)) {
      /* The app is gone or stopped reading. Close its stdin so it still gets
       * the EOF that quits it (§2) — leaving the write end open would both leak
       * the descriptor and hide the quit request — and let the reader thread's
       * stop grace end the session. */
      feed_close_stdin_locked(feed);
      feed->stop_requested = 1;
      lx_log("cannot write %s to the app; closing its stdin", what);
    }
  }
  pthread_mutex_unlock(&feed->write_lock);
  buf_release(line);
}

/*
 * App->host parsing (protocol.md §4/§7). The adapter's producer side tolerates
 * noise — a stray Qt stdout write must not break the feed — but skipping it is
 * loud (feed_log_noise()) and a line that *is* a JSON object must be a valid v2
 * message: a missing/unsupported `v`, a missing/unknown action, malformed
 * structure or an over-long line is a protocol error that ends the session
 * (feed_handle_line() below).
 */
typedef enum {
  LX_LINE_NOISE = 0, /* not a JSON object: skipped loudly, the session continues */
  LX_LINE_OK,        /* a well-formed v2 message carrying a known action */
  LX_LINE_ERROR,     /* a JSON object that violates v2 */
} lx_line_kind_t;

static const char* feed_skip_ws(const char* p)
{
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  return p;
}

/* Scans a JSON string body from its opening quote and returns the position
 * after the closing quote, or NULL when the string never closes. When @p out is
 * non-NULL the body is copied, escapes included verbatim (the v2 actions carry
 * none, and the copy is only compared against them). */
static const char* feed_scan_string(const char* p, char* out, size_t out_size)
{
  if (*p != '"') {
    return NULL;
  }
  p++;
  size_t written = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      if (out && written + 2 < out_size) {
        out[written++] = p[0];
        out[written++] = p[1];
      }
      p += 2;
      continue;
    }
    if (out && written + 1 < out_size) {
      out[written++] = *p;
    }
    p++;
  }
  if (*p != '"') {
    return NULL;
  }
  if (out) {
    out[written] = '\0';
  }
  return p + 1;
}

static lx_line_kind_t feed_parse_line(const char* line, char* action, size_t action_size,
                                      char* reason, size_t reason_size)
{
  const char* p = feed_skip_ws(line);
  if (*p != '{') {
    return LX_LINE_NOISE; /* §2: non-protocol stdout writes are tolerated here. */
  }

  bool have_version = false;
  long version = 0;
  bool have_action = false;
  action[0] = '\0';

  p = feed_skip_ws(p + 1);
  for (;;) {
    if (*p == '}') {
      p = feed_skip_ws(p + 1);
      break;
    }
    if (*p != '"') {
      snprintf(reason, reason_size, "expected a member name");
      return LX_LINE_ERROR;
    }
    char key[32];
    p = feed_scan_string(p, key, sizeof(key));
    if (p == NULL) {
      snprintf(reason, reason_size, "unterminated member name");
      return LX_LINE_ERROR;
    }
    p = feed_skip_ws(p);
    if (*p != ':') {
      snprintf(reason, reason_size, "expected ':' after \"%s\"", key);
      return LX_LINE_ERROR;
    }
    p = feed_skip_ws(p + 1);
    if (*p == '"') {
      char value[64];
      p = feed_scan_string(p, value, sizeof(value));
      if (p == NULL) {
        snprintf(reason, reason_size, "unterminated value for \"%s\"", key);
        return LX_LINE_ERROR;
      }
      if (strcmp(key, "action") == 0) {
        snprintf(action, action_size, "%s", value);
        have_action = true;
      }
    } else {
      const char* start = p;
      while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        p++;
      }
      if (p == start) {
        snprintf(reason, reason_size, "expected a value for \"%s\"", key);
        return LX_LINE_ERROR;
      }
      if (strcmp(key, "v") == 0) {
        char number[24];
        size_t length = (size_t)(p - start);
        if (length >= sizeof(number)) {
          length = sizeof(number) - 1;
        }
        memcpy(number, start, length);
        number[length] = '\0';
        char* end = NULL;
        version = strtol(number, &end, 10);
        if (end == NULL || *end != '\0') {
          snprintf(reason, reason_size, "\"v\" is not an integer");
          return LX_LINE_ERROR;
        }
        have_version = true;
      }
    }
    p = feed_skip_ws(p);
    if (*p == ',') {
      p = feed_skip_ws(p + 1);
      continue;
    }
    if (*p == '}') {
      p = feed_skip_ws(p + 1);
      break;
    }
    snprintf(reason, reason_size, "expected ',' or '}' after \"%s\"", key);
    return LX_LINE_ERROR;
  }

  if (*p != '\0') {
    snprintf(reason, reason_size, "trailing data after the JSON object");
    return LX_LINE_ERROR;
  }
  if (!have_version) {
    snprintf(reason, reason_size, "missing \"v\"");
    return LX_LINE_ERROR;
  }
  if (version != LX_FEED_PROTOCOL_VERSION) {
    snprintf(reason, reason_size, "unsupported protocol version (v=%ld)", version);
    return LX_LINE_ERROR;
  }
  if (!have_action) {
    snprintf(reason, reason_size, "missing \"action\"");
    return LX_LINE_ERROR;
  }
  return LX_LINE_OK;
}

/* Records a §7 violation: logged loudly here, reported to the host through the
 * protocol_error callback once the reader thread has stopped the session. */
static void feed_note_protocol_error(struct lx_feed* feed, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  vsnprintf(feed->protocol_error_reason, sizeof(feed->protocol_error_reason), format, args);
  va_end(args);
  lx_log("protocol error from the app: %s; ending the session", feed->protocol_error_reason);
}

/* Bytes of a tolerated non-JSON line that reach the log. */
#define LX_FEED_NOISE_LOG_BYTES 64

/* §7: skipping a non-JSON app->host line is a *loud* tolerance — one log line
 * per skipped line, with a short printable prefix (never the raw blob, and
 * never more than LX_FEED_NOISE_LOG_BYTES bytes of it). The session continues. */
static void feed_log_noise(const char* line, size_t length)
{
  char prefix[LX_FEED_NOISE_LOG_BYTES + 1];
  const size_t shown = length < LX_FEED_NOISE_LOG_BYTES ? length : LX_FEED_NOISE_LOG_BYTES;
  for (size_t i = 0; i < shown; i++) {
    const unsigned char c = (unsigned char)line[i];
    prefix[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
  }
  prefix[shown] = '\0';
  lx_log("ignoring a non-JSON app->host line: \"%s\"%s (%zu bytes)", prefix,
         length > shown ? "..." : "", length);
}

/* Returns false when @p line is a protocol violation: the session must end. */
static bool feed_handle_line(struct lx_feed* feed, const char* line, size_t length)
{
  char action[64];
  char reason[128];
  const lx_line_kind_t kind = feed_parse_line(line, action, sizeof(action), reason, sizeof(reason));
  if (kind == LX_LINE_NOISE) {
    feed_log_noise(line, length);
    return true;
  }
  if (kind == LX_LINE_ERROR) {
    feed_note_protocol_error(feed, "%s (%zu-byte line)", reason, length);
    return false;
  }

  if (strcmp(action, "close_requested") == 0) {
    if (feed->callbacks.close_requested) {
      feed->callbacks.close_requested(feed, feed->userdata);
    }
    return true;
  }
  if (strcmp(action, "get_analyser_data_array") == 0) {
    /* §4: the app may only ask a host that declared `spectrum: true`, and this
     * adapter always declares false (VLC 3.0 exposes no analyser API a plugin
     * can reach). A host that declared no analyser MUST never receive this
     * request, so it is the §7 protocol error it looks like — not an action to
     * ignore, which would leave a broken app waiting for a frame forever. */
    feed_note_protocol_error(feed, "analyser data was requested although hello.spectrum was false");
    return false;
  }
  feed_note_protocol_error(feed, "unknown action \"%s\"", action);
  return false;
}

static void* lx_feed_reader(void* arg)
{
  struct lx_feed* feed = arg;
  char chunk[4096];
  char* line = NULL;
  size_t line_len = 0;
  size_t line_cap = 0;
  bool protocol_failed = false;
  bool stop_seen = false;
  int64_t stop_at = 0;

  /* The prime: protocol §3 makes the host push its state right after the
   * handshake, and §5 makes that push a full `set_info` snapshot. The adapter's
   * sampler emits the snapshot here (the `hello` line was written before this
   * thread started, so the pipe order is right); its is_play/played_time return
   * value is ignored because `set_info` already carries both and the periodic
   * `set_status` cadence starts with the next tick. */
  if (feed->callbacks.sample_status) {
    bool is_play = false;
    int64_t played_time = 0;
    (void)feed->callbacks.sample_status(feed, feed->userdata, &is_play, &played_time);
  }
  int64_t last_status = lx_now_ms();

  for (;;) {
    struct pollfd in = {.fd = feed->from_child, .events = POLLIN, .revents = 0};
    int ready = poll(&in, 1, LX_FEED_STATUS_INTERVAL_MS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      lx_log("poll on the app's stdout failed: %s", strerror(errno));
      break;
    }
    int64_t now = lx_now_ms();
    if (feed->stop_requested) {
      if (!stop_seen) {
        stop_seen = true;
        stop_at = now;
      } else if (now - stop_at >= LX_FEED_STOP_GRACE_MS) {
        lx_log("the app did not quit after its stdin was closed");
        break;
      }
    } else if (feed->callbacks.sample_status && now - last_status >= LX_FEED_STATUS_INTERVAL_MS) {
      last_status = now;
      bool is_play = false;
      int64_t played_time = 0;
      if (feed->callbacks.sample_status(feed, feed->userdata, &is_play, &played_time)) {
        lx_feed_send_status(feed, is_play, played_time);
      }
    }
    if (ready == 0) {
      continue;
    }
    ssize_t count = read(feed->from_child, chunk, sizeof(chunk));
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      lx_log("read from the app's stdout failed: %s", strerror(errno));
      break;
    }
    if (count == 0) {
      break; /* EOF: the app exited */
    }
    for (ssize_t i = 0; i < count; i++) {
      if (chunk[i] == '\n') {
        if (!feed_handle_line(feed, line ? line : "", line_len)) {
          protocol_failed = true;
        }
        line_len = 0;
        if (protocol_failed) {
          break;
        }
        continue;
      }
      if (line_len + 2 > line_cap) {
        size_t cap = line_cap ? line_cap * 2 : 1024;
        char* grown = realloc(line, cap);
        if (grown) {
          line = grown;
          line_cap = cap;
        } else {
          lx_log("out of memory: dropped the rest of this line");
          line_len = 0;
          line_cap = 0;
          continue;
        }
      }
      if (line_len + 1 > LX_FEED_MAX_LINE) {
        /* §2/§7: a line over 1 MiB is a protocol error, not something to skip. */
        feed_note_protocol_error(feed, "line longer than %d bytes", LX_FEED_MAX_LINE);
        protocol_failed = true;
        break;
      }
      line[line_len++] = chunk[i];
      line[line_len] = '\0';
    }
    if (protocol_failed) {
      break;
    }
  }
  free(line);

  if (protocol_failed) {
    /* §7: the session is over and the child is not respawned. EOF on its stdin
     * is the quit request (§2), so it leaves on its own instead of being
     * killed; the host's stop path then reaps it. */
    feed_close_stdin(feed);
  }

  feed->child_exited = true;
  if (protocol_failed && feed->callbacks.protocol_error) {
    feed->callbacks.protocol_error(feed, feed->userdata, feed->protocol_error_reason);
  }
  if (feed->callbacks.child_exited) {
    feed->callbacks.child_exited(feed, feed->userdata);
  }
  return NULL;
}

static void feed_reap(struct lx_feed* feed)
{
  if (feed->pid <= 0) {
    return;
  }
  int status = 0;
  for (int waited = 0; waited < LX_FEED_REAP_WAIT_MS; waited += 10) {
    pid_t result = waitpid(feed->pid, &status, WNOHANG);
    if (result == feed->pid) {
      feed->pid = -1;
      return;
    }
    if (result < 0 && errno != EINTR) {
      feed->pid = -1;
      return;
    }
    struct timespec step = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
    nanosleep(&step, NULL);
  }
  lx_log("the app is still running; terminating it");
  kill(feed->pid, SIGTERM);
  for (int waited = 0; waited < LX_FEED_KILL_WAIT_MS; waited += 10) {
    pid_t result = waitpid(feed->pid, &status, WNOHANG);
    if (result == feed->pid || (result < 0 && errno != EINTR)) {
      feed->pid = -1;
      return;
    }
    struct timespec step = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
    nanosleep(&step, NULL);
  }
  /* A child that survived SIGTERM (a wedged app, or one that blocks it) must not
   * be able to hold the player's shutdown open. */
  lx_log("the app ignored SIGTERM; killing it");
  kill(feed->pid, SIGKILL);
  while (waitpid(feed->pid, &status, 0) < 0 && errno == EINTR) {
  }
  feed->pid = -1;
}

lx_feed_t* lx_feed_spawn(const char* app_path, const char* host_name,
                         const lx_feed_callbacks_t* callbacks, void* userdata)
{
  if (!app_path || !*app_path) {
    return NULL;
  }
  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};
  /* CLOEXEC so the exec'd app inherits only the two fds dup2'd onto 0 and 1 and
   * cannot itself hold the write end of its own stdin open (which would hide
   * the EOF that ends the session). */
  if (pipe2(to_child, O_CLOEXEC) != 0) {
    lx_log("cannot create the app's stdin pipe: %s", strerror(errno));
    return NULL;
  }
  if (pipe2(from_child, O_CLOEXEC) != 0) {
    lx_log("cannot create the app's stdout pipe: %s", strerror(errno));
    close(to_child[0]);
    close(to_child[1]);
    return NULL;
  }
  pid_t pid = fork();
  if (pid < 0) {
    lx_log("cannot fork: %s", strerror(errno));
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    return NULL;
  }
  if (pid == 0) {
    if (dup2(to_child[0], STDIN_FILENO) < 0 || dup2(from_child[1], STDOUT_FILENO) < 0) {
      fprintf(stderr, "lxlyrics: cannot wire the app's pipes: %s\n", strerror(errno));
      _exit(127);
    }
    /* The app is a separate application and must not inherit the host's signal
     * policy. VLC creates its threads with SIGHUP/SIGINT/SIGQUIT/SIGTERM/SIGPIPE
     * blocked around pthread_create (src/posix/thread.c:430-441), and a new
     * thread inherits the creating thread's mask — so the thread that forks here
     * has them blocked, and a blocked mask survives exec(): without this reset
     * the app would be immune to SIGTERM/SIGINT (verified: /proc/<pid>/status
     * showed SigBlk 0x15007 in the spawned app), which would also make the
     * SIGTERM escalation in feed_reap() a no-op. Ignored dispositions are left
     * as they are: they are inherited across exec by design, and the app sets
     * its own SIGPIPE policy (main.cpp ignores SIGPIPE). */
    sigset_t empty;
    sigemptyset(&empty);
    pthread_sigmask(SIG_SETMASK, &empty, NULL);
    char* const argv[] = {(char*)app_path, (char*)"--player-feed", NULL};
    execv(app_path, argv);
    fprintf(stderr, "lxlyrics: cannot execute %s: %s\n", app_path, strerror(errno));
    _exit(127);
  }
  close(to_child[0]);
  close(from_child[1]);

  struct lx_feed* feed = calloc(1, sizeof(*feed));
  if (!feed) {
    kill(pid, SIGTERM);
    close(to_child[1]);
    close(from_child[0]);
    return NULL;
  }
  feed->pid = pid;
  feed->to_child = to_child[1];
  feed->from_child = from_child[0];
  feed->stdin_open = true;
  if (callbacks) {
    feed->callbacks = *callbacks;
  }
  feed->userdata = userdata;
  /* One reference for the spawner; lx_feed_stop() drops it (see feed.h). */
  feed->refs = 1;
  pthread_mutex_init(&feed->write_lock, NULL);
  pthread_mutex_init(&feed->ref_lock, NULL);
  int flags = fcntl(feed->to_child, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(feed->to_child, F_SETFL, flags | O_NONBLOCK);
  }

  /* hello first: protocol.md §3 makes it the very first host->app line, and §4
   * makes `spectrum` false for every host without an analyser — which is this
   * one, always (see feed.h). */
  lx_buf_t hello = {0};
  buf_printf(&hello, "{\"v\":%d,\"action\":\"hello\",\"host\":\"", LX_FEED_PROTOCOL_VERSION);
  buf_put_escaped(&hello, host_name ? host_name : "");
  buf_put(&hello, "\",\"spectrum\":false}");
  feed_emit(feed, &hello, "hello");

  if (pthread_create(&feed->thread, NULL, lx_feed_reader, feed) != 0) {
    lx_log("cannot start the feed reader thread");
    /* No reader thread exists, so this is a plain teardown: close both ends,
     * reap the child, and drop the only reference. */
    feed_close_stdin(feed);
    if (feed->from_child >= 0) {
      close(feed->from_child);
      feed->from_child = -1;
    }
    kill(feed->pid, SIGTERM);
    feed_reap(feed);
    lx_feed_release(feed);
    return NULL;
  }
  feed->thread_started = true;
  return feed;
}

lx_feed_t* lx_feed_acquire(lx_feed_t* feed)
{
  if (!feed) {
    return NULL;
  }
  pthread_mutex_lock(&feed->ref_lock);
  feed->refs++;
  pthread_mutex_unlock(&feed->ref_lock);
  return feed;
}

void lx_feed_release(lx_feed_t* feed)
{
  if (!feed) {
    return;
  }
  pthread_mutex_lock(&feed->ref_lock);
  const int refs = --feed->refs;
  pthread_mutex_unlock(&feed->ref_lock);
  if (refs > 0) {
    return;
  }
  /* Last reference: the session was already torn down by lx_feed_stop(), so
   * only the resources this file owns remain. */
  pthread_mutex_destroy(&feed->ref_lock);
  pthread_mutex_destroy(&feed->write_lock);
  free(feed);
}

void lx_feed_stop(lx_feed_t* feed)
{
  if (!feed) {
    return;
  }

  /* Tear the session down exactly once, whichever thread gets here first: the
   * reader thread is joined, so no callback can still be running when the
   * spawner's reference is dropped below. */
  pthread_mutex_lock(&feed->ref_lock);
  const bool first_teardown = !feed->torn_down;
  feed->torn_down = true;
  pthread_mutex_unlock(&feed->ref_lock);

  if (first_teardown) {
    feed->stop_requested = 1;
    /* EOF on stdin is how the app is told to quit (protocol.md §2). */
    feed_close_stdin(feed);
    if (feed->thread_started) {
      pthread_join(feed->thread, NULL);
      feed->thread_started = false;
    }
    if (feed->from_child >= 0) {
      close(feed->from_child);
      feed->from_child = -1;
    }
    feed_reap(feed);
  }

  lx_feed_release(feed);
}

void lx_feed_send_info(lx_feed_t* feed, const char* path, const char* singer, const char* name,
                       const char* album, bool is_play, int64_t played_time_ms)
{
  if (!feed) {
    return;
  }
  lx_buf_t line = {0};
  buf_printf(&line, "{\"v\":%d,\"action\":\"set_info\",\"path\":\"", LX_FEED_PROTOCOL_VERSION);
  buf_put_escaped(&line, path ? path : "");
  buf_put(&line, "\",\"singer\":\"");
  buf_put_escaped(&line, singer ? singer : "");
  buf_put(&line, "\",\"name\":\"");
  buf_put_escaped(&line, name ? name : "");
  buf_put(&line, "\",\"album\":\"");
  buf_put_escaped(&line, album ? album : "");
  /* The adapter carries no lyrics: lrc stays empty so the app acquires them
   * from `path` (sidecar first, then embedded tags). */
  buf_printf(&line,
             "\",\"lrc\":\"\",\"tlrc\":\"\",\"rlrc\":\"\",\"lxlrc\":\"\",\"isPlay\":%s,"
             "\"played_time\":%lld}",
             is_play ? "true" : "false", (long long)played_time_ms);
  feed_emit(feed, &line, "set_info");
}

void lx_feed_send_status(lx_feed_t* feed, bool is_play, int64_t played_time_ms)
{
  if (!feed) {
    return;
  }
  lx_buf_t line = {0};
  buf_printf(&line, "{\"v\":%d,\"action\":\"set_status\",\"isPlay\":%s,\"played_time\":%lld}",
             LX_FEED_PROTOCOL_VERSION, is_play ? "true" : "false", (long long)played_time_ms);
  feed_emit(feed, &line, "set_status");
}

void lx_feed_send_play(lx_feed_t* feed, int64_t time_ms)
{
  if (!feed) {
    return;
  }
  lx_buf_t line = {0};
  buf_printf(&line, "{\"v\":%d,\"action\":\"set_play\",\"time\":%lld}", LX_FEED_PROTOCOL_VERSION,
             (long long)time_ms);
  feed_emit(feed, &line, "set_play");
}

void lx_feed_send_pause(lx_feed_t* feed)
{
  if (!feed) {
    return;
  }
  lx_buf_t line = {0};
  buf_printf(&line, "{\"v\":%d,\"action\":\"set_pause\"}", LX_FEED_PROTOCOL_VERSION);
  feed_emit(feed, &line, "set_pause");
}

void lx_feed_send_stop(lx_feed_t* feed)
{
  if (!feed) {
    return;
  }
  lx_buf_t line = {0};
  buf_printf(&line, "{\"v\":%d,\"action\":\"set_stop\"}", LX_FEED_PROTOCOL_VERSION);
  feed_emit(feed, &line, "set_stop");
}

void lx_feed_send_fullscreen(lx_feed_t* feed, bool is_fullscreen)
{
  if (!feed) {
    return;
  }
  lx_buf_t line = {0};
  buf_printf(&line, "{\"v\":%d,\"action\":\"set_fullscreen\",\"isFullscreen\":%s}",
             LX_FEED_PROTOCOL_VERSION, is_fullscreen ? "true" : "false");
  feed_emit(feed, &line, "set_fullscreen");
}
