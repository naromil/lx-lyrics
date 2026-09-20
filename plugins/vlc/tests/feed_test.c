/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * Hermetic tests for the protocol v2 feed writer/reader (src/feed.c + src/json.c).
 * The "app" is a POSIX shell stub, so no VLC, player or display server is
 * needed: the stub logs every host->app line it receives to $LX_STUB_LOG and
 * prints the app->host lines itself.
 */

/* Strict -std=c99 (the README's one-line build) hides struct timespec,
 * mkdtemp() and setenv(). */
#define _POSIX_C_SOURCE 200809L

#include "feed.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LX_PATH_BUF 4096

/*
 * The stub app: it logs the hello line first, then prints the app->host lines
 * (including one non-JSON line, which the adapter must tolerate) and finally
 * logs every host->app line until the host sends set_stop — exiting there is
 * what makes the adapter observe the child-exited path.
 */
static const char* const k_stub_script =
  "#!/bin/sh\n"
  "read -r hello\n"
  "printf '%s\\n' \"$hello\" >> \"$LX_STUB_LOG\"\n"
  "printf '%s\\n' 'qt.qpa.xcb: non-JSON noise the adapter must tolerate'\n"
  "printf '%s\\n' '{\"v\":2,\"action\":\"close_requested\"}'\n"
  "while IFS= read -r line; do\n"
  "  printf '%s\\n' \"$line\" >> \"$LX_STUB_LOG\"\n"
  "  case \"$line\" in\n"
  "    *'\"action\":\"set_stop\"'*) exit 0 ;;\n"
  "  esac\n"
  "done\n";

/* Prints a short non-JSON line and a 200-byte one, then drains stdin. */
static const char* const k_noise_stub_script =
  "#!/bin/sh\n"
  "printf '%s\\n' 'qt.qpa.xcb: noise line for the loud-skip test'\n"
  "i=0; while [ $i -lt 200 ]; do printf L; i=$((i + 1)); done\n"
  "printf '\\n'\n"
  "cat > /dev/null\n";

/* Ignores its stdin for long enough that the host's bounded write gives up,
 * then drains it and records that the EOF (§2) did arrive. */
static const char* const k_eof_stub_script = "#!/bin/sh\n"
                                             "sleep 2\n"
                                             "cat > /dev/null\n"
                                             "printf 'EOF_SEEN\\n' >> \"$LX_STUB_LOG\"\n";

/* Reads its stdin to EOF and exits: the fast session stub for the hammer and
 * warm-up cycles. */
static const char* const k_drain_stub_script = "#!/bin/sh\n"
                                               "cat > /dev/null\n";

/* Reports the signal mask it was started with: the adapter must hand the app a
 * clean one, never the host's (see the reset in feed.c). */
static const char* const k_mask_stub_script =
  "#!/bin/sh\n"
  "grep '^SigBlk' /proc/self/status >> \"$LX_STUB_LOG\"\n"
  "cat > /dev/null\n";

/* A wedged app: it ignores SIGTERM and never reads its stdin, so only the
 * adapter's SIGKILL escalation can end the session. It logs its pid so the test
 * can prove it is gone. */
static const char* const k_wedged_stub_script = "#!/bin/sh\n"
                                                "trap '' TERM\n"
                                                "printf 'PID %s\\n' \"$$\" >> \"$LX_STUB_LOG\"\n"
                                                "while true; do sleep 1; done\n";

/* The user-close stub: it asks the host to end the session, then reads its stdin
 * to EOF (§2), records that the host closed it, and exits by itself. */
static const char* const k_close_stub_script =
  "#!/bin/sh\n"
  "read -r hello\n"
  "printf '%s\\n' \"$hello\" >> \"$LX_STUB_LOG\"\n"
  "printf '%s\\n' '{\"v\":2,\"action\":\"close_requested\"}'\n"
  "cat >> \"$LX_STUB_LOG\"\n"
  "printf 'EOF_SEEN\\n' >> \"$LX_STUB_LOG\"\n";

/* Prints a well-formed v2 violation, then drains stdin to EOF (§2) and records
 * that the host closed it. */
static const char* const k_bad_v_stub_script =
  "#!/bin/sh\n"
  "printf '%s\\n' '{\"v\":3,\"action\":\"close_requested\"}'\n"
  "cat > \"$LX_STUB_LOG\"\n"
  "printf 'EOF_SEEN\\n' >> \"$LX_STUB_LOG\"\n";

static const char* const k_bad_action_stub_script =
  "#!/bin/sh\n"
  "printf '%s\\n' '{\"v\":2,\"action\":\"unknown_action\"}'\n"
  "cat > \"$LX_STUB_LOG\"\n"
  "printf 'EOF_SEEN\\n' >> \"$LX_STUB_LOG\"\n";

/* §4: an app must not ask for analyser data unless the hello line declared
 * `spectrum: true` — this adapter never does. */
static const char* const k_analyser_stub_script =
  "#!/bin/sh\n"
  "printf '%s\\n' '{\"v\":2,\"action\":\"get_analyser_data_array\"}'\n"
  "cat > \"$LX_STUB_LOG\"\n"
  "printf 'EOF_SEEN\\n' >> \"$LX_STUB_LOG\"\n";

/* A line over the 1 MiB protocol limit (§2/§7). The pipeline blocks once the
 * host stops reading and dies with SIGPIPE when the host closes its end, so the
 * stub must not linger (a sleeping orphan would hold the test's output pipe). */
static const char* const k_overlong_stub_script =
  "#!/bin/sh\n"
  "dd if=/dev/zero bs=1024 count=1025 2>/dev/null | tr '\\000' x\n"
  "printf '\\n'\n";

/* set_info payload: every escaping case the writer must handle, plus UTF-8. */
static const char* const k_info_path = "/tmp/a\"b\\c\td\x01.mp3";
static const char* const k_info_path_json = "/tmp/a\\\"b\\\\c\\td\\u0001.mp3";
static const char* const k_info_name = "T\xc3\xaftle\n2";
static const char* const k_info_name_json = "T\xc3\xaftle\\n2";

static int failures;
static volatile sig_atomic_t close_requests;
static volatile sig_atomic_t child_exits;
static volatile sig_atomic_t protocol_errors;
static char protocol_error_reason[256];

static void check(bool ok, const char* what)
{
  if (ok) {
    printf("ok   %s\n", what);
    return;
  }
  failures++;
  printf("FAIL %s\n", what);
}

static void check_equal(const char* what, const char* expected, const char* actual)
{
  if (actual && strcmp(expected, actual) == 0) {
    printf("ok   %s\n", what);
    return;
  }
  failures++;
  printf("FAIL %s\n  expected:\n%s  actual:\n%s", what, expected, actual ? actual : "(nothing)\n");
}

static void sleep_ms(int ms)
{
  struct timespec step = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
  nanosleep(&step, NULL);
}

static bool wait_for(volatile sig_atomic_t* counter, sig_atomic_t target, int timeout_ms)
{
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    if (*counter >= target) {
      return true;
    }
    sleep_ms(10);
  }
  return false;
}

static char* read_file(const char* path)
{
  FILE* file = fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char* text = malloc((size_t)size + 1);
  if (!text) {
    fclose(file);
    return NULL;
  }
  size_t read = fread(text, 1, (size_t)size, file);
  fclose(file);
  text[read] = '\0';
  return text;
}

static int count_lines(const char* text)
{
  int lines = 0;
  for (const char* p = text; *p; p++) {
    if (*p == '\n') {
      lines++;
    }
  }
  return lines;
}

/* Counts the lines containing @p needle. */
static int count_matching(const char* text, const char* needle)
{
  int count = 0;
  for (const char* at = text; (at = strstr(at, needle)) != NULL; at++) {
    count++;
  }
  return count;
}

static void on_close_requested(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  (void)userdata;
  close_requests++;
}

static void on_child_exited(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  (void)userdata;
  child_exits++;
}

static void on_protocol_error(lx_feed_t* feed, void* userdata, const char* reason)
{
  (void)feed;
  (void)userdata;
  snprintf(protocol_error_reason, sizeof(protocol_error_reason), "%s", reason ? reason : "");
  protocol_errors++;
}

/* Polls @p path until @p needle shows up (or the timeout expires). */
static bool wait_for_text(const char* path, const char* needle, int timeout_ms)
{
  for (int waited = 0; waited <= timeout_ms; waited += 20) {
    char* text = read_file(path);
    const bool seen = text != NULL && strstr(text, needle) != NULL;
    free(text);
    if (seen) {
      return true;
    }
    sleep_ms(20);
  }
  return false;
}

/* Proof that the host closed the app's stdin (protocol.md §2) rather than
 * leaving the pipe open. */
static bool wait_for_eof_marker(const char* path, int timeout_ms)
{
  return wait_for_text(path, "EOF_SEEN", timeout_ms);
}

/*
 * The adapter's prime, as the plugin implements it: the feed's first sampler
 * call pushes the full snapshot (set_info + the state action + set_fullscreen)
 * right after the handshake. Returning false means "no periodic set_status", so
 * this test's log is deterministic.
 */
static bool on_prime_status(lx_feed_t* feed, void* userdata, bool* is_play, int64_t* played_time_ms)
{
  (void)userdata;
  (void)is_play;
  (void)played_time_ms;
  lx_feed_send_info(feed, "/music/prime.mp3", "Prime Singer", "Prime Song", "Prime Album", true,
                    1000);
  lx_feed_send_play(feed, 1000);
  lx_feed_send_fullscreen(feed, true);
  return false;
}

/* The periodic clock: always playing at 5000 ms, so every tick emits set_status. */
static bool on_periodic_status(lx_feed_t* feed, void* userdata, bool* is_play,
                               int64_t* played_time_ms)
{
  (void)feed;
  (void)userdata;
  *is_play = true;
  *played_time_ms = 5000;
  return true;
}

/* The exact host->app bytes: the vlc handshake, the snapshot the prime pushes
 * immediately after it (protocol §3/§5), then each writer's line, with the
 * escaping the JSON contract requires. */
static void test_handshake_and_snapshot(const char* script, const char* log_path)
{
  close_requests = 0;
  child_exits = 0;
  unlink(log_path);
  setenv("LX_STUB_LOG", log_path, 1);

  const lx_feed_callbacks_t callbacks = {
    .close_requested = on_close_requested,
    .child_exited = on_child_exited,
    .sample_status = on_prime_status,
  };
  lx_feed_t* feed = lx_feed_spawn(script, "vlc", &callbacks, NULL);
  check(feed != NULL, "spawn the stub app");
  if (!feed) {
    return;
  }
  /* The stub prints close_requested after the host's prime has been written (the
   * reader thread primes before it reads anything), so waiting for it makes the
   * log's order deterministic. */
  check(wait_for(&close_requests, 1, 2000), "the app's close_requested is parsed");

  lx_feed_send_info(feed, k_info_path, "Singer", k_info_name, "Album", true, 1234);
  lx_feed_send_status(feed, true, 2345);
  lx_feed_send_play(feed, 3456);
  lx_feed_send_pause(feed);
  lx_feed_send_fullscreen(feed, false);
  lx_feed_send_stop(feed);
  check(wait_for(&child_exits, 1, 3000), "the app exiting is reported");
  lx_feed_stop(feed);
  check(close_requests == 1, "close_requested is reported exactly once");
  check(protocol_errors == 0, "no protocol error is reported");

  char expected[8192];
  snprintf(expected, sizeof(expected),
           "{\"v\":2,\"action\":\"hello\",\"host\":\"vlc\",\"spectrum\":false}\n"
           "{\"v\":2,\"action\":\"set_info\",\"path\":\"/music/prime.mp3\","
           "\"singer\":\"Prime Singer\",\"name\":\"Prime Song\",\"album\":\"Prime Album\","
           "\"lrc\":\"\",\"tlrc\":\"\",\"rlrc\":\"\",\"lxlrc\":\"\",\"isPlay\":true,"
           "\"played_time\":1000}\n"
           "{\"v\":2,\"action\":\"set_play\",\"time\":1000}\n"
           "{\"v\":2,\"action\":\"set_fullscreen\",\"isFullscreen\":true}\n"
           "{\"v\":2,\"action\":\"set_info\",\"path\":\"%s\",\"singer\":\"Singer\","
           "\"name\":\"%s\",\"album\":\"Album\",\"lrc\":\"\",\"tlrc\":\"\",\"rlrc\":\"\","
           "\"lxlrc\":\"\",\"isPlay\":true,\"played_time\":1234}\n"
           "{\"v\":2,\"action\":\"set_status\",\"isPlay\":true,\"played_time\":2345}\n"
           "{\"v\":2,\"action\":\"set_play\",\"time\":3456}\n"
           "{\"v\":2,\"action\":\"set_pause\"}\n"
           "{\"v\":2,\"action\":\"set_fullscreen\",\"isFullscreen\":false}\n"
           "{\"v\":2,\"action\":\"set_stop\"}\n",
           k_info_path_json, k_info_name_json);
  char* log = read_file(log_path);
  if (!log) {
    check(false, "the stub app logged the host->app lines");
    return;
  }
  check_equal("the handshake, the snapshot and every host->app line, in order", expected, log);
  free(log);
}

/* The status clock: hello first, then only periodic set_status lines — and no
 * set_status on the prime itself (its set_info already carries state+position). */
static void test_periodic_status(const char* script, const char* log_path)
{
  child_exits = 0;
  unlink(log_path);
  setenv("LX_STUB_LOG", log_path, 1);

  const lx_feed_callbacks_t callbacks = {
    .child_exited = on_child_exited,
    .sample_status = on_periodic_status,
  };
  lx_feed_t* feed = lx_feed_spawn(script, "vlc", &callbacks, NULL);
  check(feed != NULL, "spawn the periodic-status stub");
  if (!feed) {
    return;
  }
  bool saw_status = false;
  for (int waited = 0; waited < 2500 && !saw_status; waited += 20) {
    char* probe = read_file(log_path);
    saw_status = probe && count_lines(probe) >= 3;
    free(probe);
    if (!saw_status) {
      sleep_ms(20);
    }
  }
  check(saw_status, "the adapter pushes set_status while the track plays");
  lx_feed_stop(feed);

  char* log = read_file(log_path);
  if (!log) {
    check(false, "the stub app logged the host->app lines");
    return;
  }
  int index = 0;
  bool lines_ok = true;
  char* cursor = log;
  while (*cursor) {
    char* end = strchr(cursor, '\n');
    if (!end) {
      break;
    }
    *end = '\0';
    index++;
    const char* expected =
      index == 1 ? "{\"v\":2,\"action\":\"hello\",\"host\":\"vlc\",\"spectrum\":false}"
                 : "{\"v\":2,\"action\":\"set_status\",\"isPlay\":true,\"played_time\":5000}";
    lines_ok = lines_ok && strcmp(cursor, expected) == 0;
    cursor = end + 1;
  }
  check(index >= 3 && lines_ok, "hello first, then only periodic set_status lines");
  free(log);
}

/* Number of descriptors this process holds (includes /proc's own dir handle). */
static int open_fd_count(void)
{
  DIR* dir = opendir("/proc/self/fd");
  if (!dir) {
    return -1;
  }
  int count = 0;
  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] != '.') {
      count++;
    }
  }
  closedir(dir);
  return count;
}

/* A session owns four pipe ends and a child process: a leak here would
 * accumulate one per VLC start for the whole run. */
static void test_no_fd_leak(const char* script, const char* log_path)
{
  setenv("LX_STUB_LOG", log_path, 1);
  const lx_feed_callbacks_t callbacks = {.child_exited = on_child_exited};
  lx_feed_t* warmup = lx_feed_spawn(script, "vlc", &callbacks, NULL);
  if (!warmup) {
    check(false, "spawn a session for the descriptor check");
    return;
  }
  lx_feed_stop(warmup);
  int baseline = open_fd_count();
  if (baseline < 0) {
    check(false, "count the open descriptors");
    return;
  }
  for (int cycle = 0; cycle < 3; cycle++) {
    lx_feed_t* feed = lx_feed_spawn(script, "vlc", &callbacks, NULL);
    if (!feed) {
      check(false, "spawn a session in the descriptor loop");
      return;
    }
    lx_feed_stop(feed);
  }
  check(open_fd_count() == baseline, "start/stop cycles leak no file descriptors");
}

static void test_missing_app(void)
{
  child_exits = 0;
  check(lx_feed_spawn(NULL, "vlc", NULL, NULL) == NULL, "spawn without an app path is refused");
  check(lx_feed_spawn("", "vlc", NULL, NULL) == NULL, "spawn with an empty app path is refused");

  /* The plugin calls these with no session running: they must be no-ops. */
  lx_feed_send_info(NULL, "p", "s", "n", "a", true, 1);
  lx_feed_send_status(NULL, true, 1);
  lx_feed_send_play(NULL, 1);
  lx_feed_send_pause(NULL);
  lx_feed_send_stop(NULL);
  lx_feed_send_fullscreen(NULL, true);
  lx_feed_stop(NULL);
  check(true, "the writers tolerate a NULL feed");

  const lx_feed_callbacks_t callbacks = {.child_exited = on_child_exited};
  lx_feed_t* feed = lx_feed_spawn("/nonexistent/lx-lyrics-app", "vlc", &callbacks, NULL);
  check(feed != NULL, "spawn reports a feed even when the app cannot be executed");
  check(wait_for(&child_exits, 1, 3000), "a failed exec still ends the session");
  lx_feed_stop(feed);
}

/* §7: a well-formed v2 violation ends the session — protocol_error fires once
 * with a reason, the app is told to quit by closing its stdin (§2) and the
 * session ends. No respawn, no stale state. */
static void test_protocol_error(const char* what, const char* script, const char* log_path,
                                bool expect_eof)
{
  protocol_errors = 0;
  child_exits = 0;
  protocol_error_reason[0] = '\0';
  unlink(log_path);
  setenv("LX_STUB_LOG", log_path, 1);

  const lx_feed_callbacks_t callbacks = {
    .protocol_error = on_protocol_error,
    .child_exited = on_child_exited,
  };
  lx_feed_t* feed = lx_feed_spawn(script, "vlc", &callbacks, NULL);
  check(feed != NULL, what);
  if (!feed) {
    return;
  }
  check(wait_for(&protocol_errors, 1, 3000), "the violation is reported as a protocol error");
  check(protocol_error_reason[0] != '\0', "the protocol error carries a reason");
  check(wait_for(&child_exits, 1, 3000), "the violating session ends");
  lx_feed_stop(feed);
  check(protocol_errors == 1, "the violation is reported exactly once");
  if (expect_eof) {
    check(wait_for_eof_marker(log_path, 3000), "the app got EOF on its stdin and quit by itself");
  }
}

/*
 * The user closed the lyric window (protocol §4). This mirrors the adapter's
 * shape exactly: one thread owns the session — it spawns the feed, waits for a
 * callback to ask for the end, and stops the feed from that same thread (feed.h)
 * — so the read-back, the EOF the app quits on and the absence of a respawn are
 * all covered.
 */
typedef struct {
  const char* app_path;
  lx_feed_t* feed;
  pthread_mutex_t lock;
  pthread_cond_t wait;
  bool end_requested;
} session_t;

static void session_request_end(session_t* session)
{
  pthread_mutex_lock(&session->lock);
  session->end_requested = true;
  pthread_cond_signal(&session->wait);
  pthread_mutex_unlock(&session->lock);
}

static void on_session_close_requested(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  close_requests++;
  session_request_end(userdata);
}

static void on_session_child_exited(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  child_exits++;
  session_request_end(userdata);
}

static void* session_thread(void* arg)
{
  session_t* session = arg;
  const lx_feed_callbacks_t callbacks = {
    .close_requested = on_session_close_requested,
    .child_exited = on_session_child_exited,
  };
  lx_feed_t* feed = lx_feed_spawn(session->app_path, "vlc", &callbacks, session);
  pthread_mutex_lock(&session->lock);
  session->feed = feed;
  while (!session->end_requested) {
    pthread_cond_wait(&session->wait, &session->lock);
  }
  session->feed = NULL;
  pthread_mutex_unlock(&session->lock);
  lx_feed_stop(feed);
  return NULL;
}

static void test_close_requested_ends_the_session(const char* script, const char* log_path)
{
  close_requests = 0;
  child_exits = 0;
  protocol_errors = 0;
  unlink(log_path);
  setenv("LX_STUB_LOG", log_path, 1);

  session_t session;
  memset(&session, 0, sizeof(session));
  session.app_path = script;
  pthread_mutex_init(&session.lock, NULL);
  pthread_cond_init(&session.wait, NULL);

  pthread_t thread;
  const bool started = pthread_create(&thread, NULL, session_thread, &session) == 0;
  check(started, "start the session thread");
  if (!started) {
    return;
  }
  check(wait_for(&close_requests, 1, 3000), "close_requested is read back from the app");
  check(wait_for(&child_exits, 1, 4000), "the session ends once the app quits on the EOF");
  check(wait_for_eof_marker(log_path, 3000), "the app got EOF on its stdin and quit by itself");
  pthread_join(thread, NULL);

  check(close_requests == 1, "close_requested is reported exactly once");
  check(child_exits == 1, "the child exit is reported exactly once");
  check(protocol_errors == 0, "a user close is not a protocol error");

  char* log = read_file(log_path);
  if (!log) {
    check(false, "the stub app logged the host->app lines");
    return;
  }
  /* Exactly one handshake and one EOF: nothing was respawned after the close. */
  check(count_matching(log, "\"action\":\"hello\"") == 1, "the session is not respawned");
  check(count_matching(log, "EOF_SEEN") == 1, "the app's stdin was closed exactly once");
  free(log);

  pthread_cond_destroy(&session.wait);
  pthread_mutex_destroy(&session.lock);
}

/* The host gives up on a full stdin pipe: it must close the write end (so the
 * app still sees the EOF that quits it, §2) and must not leak the descriptor. */
static void test_failed_write_closes_stdin(const char* eof_stub, const char* drain_stub,
                                           const char* log_path)
{
  child_exits = 0;
  unlink(log_path);
  setenv("LX_STUB_LOG", log_path, 1);
  const lx_feed_callbacks_t callbacks = {.child_exited = on_child_exited};

  /* Warm up so the baseline is taken with the same fds the harness already holds. */
  lx_feed_t* warm = lx_feed_spawn(drain_stub, "vlc", &callbacks, NULL);
  if (!warm) {
    check(false, "spawn the warm-up session");
    return;
  }
  lx_feed_stop(warm);
  const int baseline = open_fd_count();
  if (baseline < 0) {
    check(false, "count the open descriptors");
    return;
  }

  lx_feed_t* feed = lx_feed_spawn(eof_stub, "vlc", &callbacks, NULL);
  if (!feed) {
    check(false, "spawn the failed-write session");
    return;
  }
  char* huge = malloc(200001);
  if (!huge) {
    check(false, "allocate the oversized metadata");
    lx_feed_stop(feed);
    return;
  }
  memset(huge, 'x', 200000);
  huge[200000] = '\0';
  /* Larger than the pipe buffer, and the stub is not reading: the bounded write
   * gives up and the give-up path must close the app's stdin. */
  lx_feed_send_info(feed, "/tmp/x.mp3", "s", huge, "a", true, 0);
  free(huge);

  check(wait_for_eof_marker(log_path, 6000), "the app sees EOF after the host gave up writing");
  check(wait_for(&child_exits, 1, 4000), "the session ends once the app quits on that EOF");
  lx_feed_stop(feed);
  check(open_fd_count() == baseline, "a failed write leaks no file descriptor");
}

typedef struct {
  lx_feed_t* feed;
  volatile sig_atomic_t stop;
} hammer_t;

static void* hammer_writer(void* arg)
{
  hammer_t* hammer = arg;
  while (!hammer->stop) {
    lx_feed_send_status(hammer->feed, true, 1000);
    lx_feed_send_info(hammer->feed, "/tmp/hammer.mp3", "singer", "name", "album", true, 1000);
    lx_feed_send_fullscreen(hammer->feed, true);
  }
  return NULL;
}

/* One thread hammers the writers while the session is ended from another: the
 * feed must stay alive for the writer's own reference (feed.h) and the writes
 * must degrade to no-ops — never a use-after-free (run this under ASAN) and
 * never a descriptor leak. */
static void test_concurrent_stop(const char* drain_stub)
{
  const lx_feed_callbacks_t callbacks = {.child_exited = on_child_exited};
  lx_feed_t* warm = lx_feed_spawn(drain_stub, "vlc", &callbacks, NULL);
  if (!warm) {
    check(false, "spawn a warm-up session for the concurrency check");
    return;
  }
  lx_feed_stop(warm);
  const int baseline = open_fd_count();
  if (baseline < 0) {
    check(false, "count the open descriptors");
    return;
  }

  bool threads_ok = true;
  for (int cycle = 0; cycle < 10 && threads_ok; cycle++) {
    lx_feed_t* feed = lx_feed_spawn(drain_stub, "vlc", &callbacks, NULL);
    if (!feed) {
      check(false, "spawn a session for the concurrency check");
      return;
    }
    /* The writer thread's own reference: it outlives the stop below. */
    lx_feed_t* held = lx_feed_acquire(feed);
    hammer_t hammer = {.feed = held, .stop = 0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, hammer_writer, &hammer) != 0) {
      check(false, "start the hammer thread");
      lx_feed_release(held);
      lx_feed_stop(feed);
      return;
    }
    sleep_ms(2);
    lx_feed_stop(feed); /* ends the session under the running writer */
    hammer.stop = 1;
    threads_ok = pthread_join(thread, NULL) == 0;
    /* The session is over; this reference must still be usable as a no-op. */
    lx_feed_send_status(held, true, 1);
    lx_feed_send_info(held, "/tmp/x.mp3", "s", "n", "a", true, 1);
    lx_feed_send_fullscreen(held, false);
    lx_feed_release(held);
  }
  check(threads_ok, "writers keep running while the session is stopped");
  check(open_fd_count() == baseline, "stop cycles with a concurrent writer leak no descriptors");
}

/* §7: skipping a non-JSON line is a LOUD tolerance — one bounded, printable log
 * line per skipped line, and the session carries on. The adapter's own stderr is
 * captured around the session to read what it logged. */
static void test_noise_is_logged_loudly(const char* noise_stub, const char* capture_path)
{
  unlink(capture_path);
  fflush(stderr);
  const int saved = dup(STDERR_FILENO);
  const int fd = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (saved < 0 || fd < 0) {
    check(false, "capture the adapter's stderr");
    if (saved >= 0) {
      close(saved);
    }
    if (fd >= 0) {
      close(fd);
    }
    return;
  }
  dup2(fd, STDERR_FILENO);
  close(fd);

  /* The bounded form of the 200-byte line: 64 printable bytes, then the closing
   * quote and an ellipsis — never the raw blob (a 65-byte run of the padding can
   * only come from it). */
  char expected[80];
  memset(expected, 'L', 64);
  snprintf(expected + 64, sizeof(expected) - 64, "\"...");
  char runaway[66];
  memset(runaway, 'L', 65);
  runaway[65] = '\0';

  const lx_feed_callbacks_t callbacks = {.child_exited = on_child_exited};
  lx_feed_t* feed = lx_feed_spawn(noise_stub, "vlc", &callbacks, NULL);
  const bool spawned = feed != NULL;
  if (feed) {
    /* The stub prints both lines as soon as it is up; wait for the truncated
     * form (which can only be logged after the short line) before stopping. */
    (void)wait_for_text(capture_path, expected, 2000);
    lx_feed_stop(feed);
  }

  fflush(stderr);
  dup2(saved, STDERR_FILENO);
  close(saved);

  check(spawned, "spawn a stub that prints non-JSON lines");
  char* text = read_file(capture_path);
  if (!text) {
    check(false, "read the captured stderr");
    return;
  }

  /* One log line per skipped line, naming the tolerance and carrying the text. */
  check(count_matching(text, "ignoring a non-JSON app->host line") == 2,
        "each tolerated non-JSON line is logged exactly once");
  check(strstr(text, "noise line for the loud-skip test") != NULL,
        "the log carries the skipped line's text");
  check(strstr(text, expected) != NULL, "the log is truncated to a short printable prefix");
  check(strstr(text, runaway) == NULL, "the log never carries the untruncated line");
  free(text);
}

/* VLC creates its threads with SIGHUP/SIGINT/SIGQUIT/SIGTERM/SIGPIPE blocked
 * around pthread_create (src/posix/thread.c:430-441) and a new thread inherits
 * the creating thread's mask, so the adapter's session thread forks the app with
 * those blocked. A blocked mask survives exec(), so feed.c resets it in the
 * child — without that the app could not be signalled at all. This spawns from a
 * thread carrying exactly VLC's mask and reads back what the app started with. */
typedef struct {
  const char* app_path;
  const char* log_path;
} masked_spawn_t;

static void* masked_spawn_thread(void* arg)
{
  const masked_spawn_t* spawn = arg;
  sigset_t set;
  sigset_t previous;
  sigemptyset(&set);
  sigaddset(&set, SIGHUP);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGQUIT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, &previous); /* what vlc_clone_attr() does */

  const lx_feed_callbacks_t callbacks = {.child_exited = on_child_exited};
  lx_feed_t* feed = lx_feed_spawn(spawn->app_path, "vlc", &callbacks, NULL);
  if (feed) {
    (void)wait_for_text(spawn->log_path, "SigBlk", 2000);
    lx_feed_stop(feed);
  }
  pthread_sigmask(SIG_SETMASK, &previous, NULL);
  return NULL;
}

static void test_child_signal_mask(const char* script, const char* log_path)
{
  child_exits = 0;
  unlink(log_path);
  setenv("LX_STUB_LOG", log_path, 1);

  masked_spawn_t spawn = {.app_path = script, .log_path = log_path};
  pthread_t thread;
  const bool started = pthread_create(&thread, NULL, masked_spawn_thread, &spawn) == 0;
  check(started, "start a spawner thread with the host's signal mask");
  if (!started) {
    return;
  }
  pthread_join(thread, NULL);

  char* log = read_file(log_path);
  if (!log) {
    check(false, "the stub app reported its signal mask");
    return;
  }
  /* /proc/self/status: "SigBlk:\t0000000000000000" when nothing is blocked. */
  check(strstr(log, "SigBlk:") != NULL, "the stub app reported its signal mask");
  check(strstr(log, "SigBlk:\t0000000000000000") != NULL,
        "the app is spawned with an empty signal mask, not the host's");
  free(log);
}

/* A wedged app must not be able to hold the player open: the adapter gives up on
 * its stdin, asks it to leave, and kills it if it will not. */
static void test_wedged_app_is_killed(const char* script, const char* log_path)
{
  child_exits = 0;
  unlink(log_path);
  setenv("LX_STUB_LOG", log_path, 1);

  const lx_feed_callbacks_t callbacks = {.child_exited = on_child_exited};
  lx_feed_t* feed = lx_feed_spawn(script, "vlc", &callbacks, NULL);
  check(feed != NULL, "spawn a stub that ignores SIGTERM");
  if (!feed) {
    return;
  }
  check(wait_for_text(log_path, "PID ", 2000), "the wedged stub reported its pid");

  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  lx_feed_stop(feed); /* closes stdin, then escalates: SIGTERM, then SIGKILL */
  clock_gettime(CLOCK_MONOTONIC, &end);
  const int64_t elapsed_ms =
    (int64_t)(end.tv_sec - start.tv_sec) * 1000 + (int64_t)(end.tv_nsec - start.tv_nsec) / 1000000;
  check(elapsed_ms < 8000, "stopping a wedged app is bounded");
  /* The adapter first gives the app the feed's stop grace (2500 ms) to leave on
   * the EOF it was sent, and only then escalates. */
  check(elapsed_ms >= 2000, "the adapter waited for the app to leave before escalating");

  char* log = read_file(log_path);
  if (!log) {
    check(false, "the wedged stub reported its pid");
    return;
  }
  char* pid_text = strstr(log, "PID ");
  pid_t pid = -1;
  if (pid_text) {
    pid = (pid_t)strtol(pid_text + 4, NULL, 10);
  }
  free(log);
  check(pid > 0, "the wedged stub's pid was read");
  if (pid > 0) {
    /* ESRCH: the escalation killed it (a SIGTERM-immune shell would still be
     * there otherwise). */
    check(kill(pid, 0) != 0 && errno == ESRCH, "the wedged app was killed outright");
  }
}

/* Writes an executable stub script. */
static bool write_stub(const char* path, const char* body)
{
  FILE* file = fopen(path, "wb");
  if (!file) {
    return false;
  }
  const bool ok = fputs(body, file) >= 0;
  return fclose(file) == 0 && ok && chmod(path, 0755) == 0;
}

int main(void)
{
  char dir[] = "/tmp/lx-vlc-feed-test-XXXXXX";
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    return 1;
  }
  char script[LX_PATH_BUF];
  char drain_script[LX_PATH_BUF];
  char eof_script[LX_PATH_BUF];
  char noise_script[LX_PATH_BUF];
  char close_script[LX_PATH_BUF];
  char mask_script[LX_PATH_BUF];
  char wedged_script[LX_PATH_BUF];
  char bad_v_script[LX_PATH_BUF];
  char bad_action_script[LX_PATH_BUF];
  char analyser_script[LX_PATH_BUF];
  char overlong_script[LX_PATH_BUF];
  char scripted_log[LX_PATH_BUF];
  char periodic_log[LX_PATH_BUF];
  char leak_log[LX_PATH_BUF];
  char close_log[LX_PATH_BUF];
  char bad_v_log[LX_PATH_BUF];
  char bad_action_log[LX_PATH_BUF];
  char analyser_log[LX_PATH_BUF];
  char overlong_log[LX_PATH_BUF];
  char eof_log[LX_PATH_BUF];
  char noise_capture[LX_PATH_BUF];
  char mask_log[LX_PATH_BUF];
  char wedged_log[LX_PATH_BUF];
  snprintf(script, sizeof(script), "%s/stub-feed.sh", dir);
  snprintf(drain_script, sizeof(drain_script), "%s/stub-drain.sh", dir);
  snprintf(eof_script, sizeof(eof_script), "%s/stub-eof.sh", dir);
  snprintf(noise_script, sizeof(noise_script), "%s/stub-noise.sh", dir);
  snprintf(close_script, sizeof(close_script), "%s/stub-close.sh", dir);
  snprintf(mask_script, sizeof(mask_script), "%s/stub-mask.sh", dir);
  snprintf(wedged_script, sizeof(wedged_script), "%s/stub-wedged.sh", dir);
  snprintf(bad_v_script, sizeof(bad_v_script), "%s/stub-bad-v.sh", dir);
  snprintf(bad_action_script, sizeof(bad_action_script), "%s/stub-bad-action.sh", dir);
  snprintf(analyser_script, sizeof(analyser_script), "%s/stub-analyser.sh", dir);
  snprintf(overlong_script, sizeof(overlong_script), "%s/stub-overlong.sh", dir);
  snprintf(scripted_log, sizeof(scripted_log), "%s/scripted.log", dir);
  snprintf(periodic_log, sizeof(periodic_log), "%s/periodic.log", dir);
  snprintf(leak_log, sizeof(leak_log), "%s/leak.log", dir);
  snprintf(close_log, sizeof(close_log), "%s/close.log", dir);
  snprintf(bad_v_log, sizeof(bad_v_log), "%s/bad-v.log", dir);
  snprintf(bad_action_log, sizeof(bad_action_log), "%s/bad-action.log", dir);
  snprintf(analyser_log, sizeof(analyser_log), "%s/analyser.log", dir);
  snprintf(overlong_log, sizeof(overlong_log), "%s/overlong.log", dir);
  snprintf(eof_log, sizeof(eof_log), "%s/eof.log", dir);
  snprintf(noise_capture, sizeof(noise_capture), "%s/noise-capture.log", dir);
  snprintf(mask_log, sizeof(mask_log), "%s/mask.log", dir);
  snprintf(wedged_log, sizeof(wedged_log), "%s/wedged.log", dir);

  const char* const stub_paths[] = {
    script,        drain_script, eof_script,        noise_script,    close_script,    mask_script,
    wedged_script, bad_v_script, bad_action_script, analyser_script, overlong_script,
  };
  const char* const stub_bodies[] = {
    k_stub_script,          k_drain_stub_script,    k_eof_stub_script,
    k_noise_stub_script,    k_close_stub_script,    k_mask_stub_script,
    k_wedged_stub_script,   k_bad_v_stub_script,    k_bad_action_stub_script,
    k_analyser_stub_script, k_overlong_stub_script,
  };
  for (size_t i = 0; i < sizeof(stub_paths) / sizeof(stub_paths[0]); i++) {
    if (!write_stub(stub_paths[i], stub_bodies[i])) {
      perror("the stub app");
      return 1;
    }
  }

  test_handshake_and_snapshot(script, scripted_log);
  test_periodic_status(script, periodic_log);
  test_missing_app();
  test_no_fd_leak(script, leak_log);
  test_close_requested_ends_the_session(close_script, close_log);
  test_protocol_error("a stub with an unsupported protocol version", bad_v_script, bad_v_log, true);
  test_protocol_error("a stub with an unknown action", bad_action_script, bad_action_log, true);
  test_protocol_error("a stub asking for analyser data although hello.spectrum is false",
                      analyser_script, analyser_log, true);
  test_protocol_error("a stub with an over-long line", overlong_script, overlong_log, false);
  test_failed_write_closes_stdin(eof_script, drain_script, eof_log);
  test_child_signal_mask(mask_script, mask_log);
  test_wedged_app_is_killed(wedged_script, wedged_log);
  test_concurrent_stop(drain_script);
  test_noise_is_logged_loudly(noise_script, noise_capture);

  for (size_t i = 0; i < sizeof(stub_paths) / sizeof(stub_paths[0]); i++) {
    unlink(stub_paths[i]);
  }
  unlink(scripted_log);
  unlink(periodic_log);
  unlink(leak_log);
  unlink(close_log);
  unlink(bad_v_log);
  unlink(bad_action_log);
  unlink(analyser_log);
  unlink(overlong_log);
  unlink(eof_log);
  unlink(noise_capture);
  unlink(mask_log);
  unlink(wedged_log);
  rmdir(dir);
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
