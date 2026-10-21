/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * Hermetic tests for the global-shortcut transport (src/hotkey.c): /bin/sh
 * stand-ins speak the helper's protocol on the inherited pipe, so the spawn
 * argv/fd contract, line delivery, EOF handling and stop/reap semantics are
 * asserted with no GLib, no player and no display server.
 */

#include "hotkey.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static atomic_int toggles;
static atomic_int statuses;
/* Every status line the helper wrote, newline-joined. */
static char history[2048];
static void on_toggle(void* userdata)
{
  (void)userdata;
  atomic_fetch_add(&toggles, 1);
}

static void on_status(const char* line, void* userdata)
{
  (void)userdata;
  const size_t used = strlen(history);
  if (used + 1 < sizeof(history)) {
    snprintf(history + used, sizeof(history) - used, "%s\n", line ? line : "");
  }
  atomic_fetch_add(&statuses, 1);
}

static void reset_counters(void)
{
  atomic_store(&toggles, 0);
  atomic_store(&statuses, 0);
  history[0] = '\0';
}

static char* write_script(const char* dir, const char* name, const char* body, mode_t mode)
{
  char* path = malloc(strlen(dir) + strlen(name) + 2);
  sprintf(path, "%s/%s", dir, name);
  FILE* file = fopen(path, "w");
  if (!file) {
    perror("fopen");
    exit(1);
  }
  fputs(body, file);
  fclose(file);
  chmod(path, mode);
  return path;
}

static void wait_for(atomic_int* counter, int target, int timeout_ms)
{
  for (int waited = 0; waited < timeout_ms && atomic_load(counter) < target; waited += 10) {
    usleep(10 * 1000);
  }
}

static long long now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 1. The spawn contract: fd 3 is the pipe and argv carries the accelerator;
 *    every complete line is delivered verbatim, "toggle" calls the callback
 *    once per line, and stop() reaps the still-running helper. */
static void test_spawn_and_delivery(const char* dir)
{
  char* script = write_script(dir, "chatty.sh",
                              "#!/bin/sh\n"
                              "printf 'args %s\\n' \"$*\" >&3\n"
                              "printf 'ready CTRL+ALT+l\\n' >&3\n"
                              "sleep 0.1\n"
                              "printf 'toggle\\n' >&3\n"
                              "sleep 0.1\n"
                              "printf 'toggle\\n' >&3\n"
                              "printf 'toggle now\\n' >&3\n"
                              "sleep 10\n",
                              0755);
  reset_counters();
  const lx_hotkey_config_t config = {
    .helper_path = script,
    .accelerator = "CTRL+ALT+l",
    .on_toggle = on_toggle,
    .on_status = on_status,
  };
  lx_hotkey_t* hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  if (!hotkey) {
    unlink(script);
    free(script);
    return;
  }

  /* args line + ready line + "toggle now" (not the exact toggle line). */
  wait_for(&statuses, 3, 5000);
  wait_for(&toggles, 2, 5000);
  CHECK(atomic_load(&statuses) == 3);
  CHECK(atomic_load(&toggles) == 2);
  CHECK(strstr(history, "--pipe-fd 3") != NULL);
  CHECK(strstr(history, "--accelerator CTRL+ALT+l") != NULL);
  CHECK(strstr(history, "ready CTRL+ALT+l") != NULL);
  CHECK(strstr(history, "toggle now") != NULL);
  CHECK(strstr(history, "toggle\n") == NULL); /* the exact toggles never reach on_status */

  const long long started = now_ms();
  lx_hotkey_stop(hotkey);
  CHECK(now_ms() - started < 3000);
  CHECK(waitpid(-1, NULL, WNOHANG) < 0); /* the helper was reaped, no zombie */

  unlink(script);
  free(script);
}

/* 2. An unavailable line is delivered once and toggles nothing. */
static void test_unavailable(const char* dir)
{
  char* script = write_script(dir, "unavailable.sh",
                              "#!/bin/sh\n"
                              "printf 'unavailable An app id is required\\n' >&3\n"
                              "sleep 10\n",
                              0755);
  reset_counters();
  const lx_hotkey_config_t config = {
    .helper_path = script,
    .accelerator = NULL,
    .on_toggle = on_toggle,
    .on_status = on_status,
  };
  lx_hotkey_t* hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  if (hotkey) {
    wait_for(&statuses, 1, 5000);
    CHECK(atomic_load(&statuses) == 1);
    CHECK(strcmp(history, "unavailable An app id is required\n") == 0);
    CHECK(atomic_load(&toggles) == 0);
    lx_hotkey_stop(hotkey);
  }
  unlink(script);
  free(script);
}

/* 3. A helper that exits on its own ends the reader (no spin, no hang) and
 *    stop() stays prompt; stop() is also idempotent. */
static void test_self_exit_and_idempotent_stop(const char* dir)
{
  char* script = write_script(dir, "exit.sh",
                              "#!/bin/sh\n"
                              "printf 'ready once\\n' >&3\n"
                              "exit 0\n",
                              0755);
  reset_counters();
  const lx_hotkey_config_t config = {
    .helper_path = script,
    .accelerator = "",
    .on_toggle = on_toggle,
    .on_status = on_status,
  };
  lx_hotkey_t* hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  if (hotkey) {
    wait_for(&statuses, 1, 5000);
    CHECK(atomic_load(&statuses) == 1);
    CHECK(strstr(history, "ready once") != NULL);
    /* No --accelerator argument when the preferred trigger is empty. */
    const long long started = now_ms();
    lx_hotkey_stop(hotkey);
    lx_hotkey_stop(hotkey); /* idempotent */
    CHECK(now_ms() - started < 3000);
  }
  unlink(script);
  free(script);
}

/* 4. A helper that never writes: stop() must still terminate and reap it. */
static void test_mute_helper(const char* dir)
{
  char* script = write_script(dir, "mute.sh", "#!/bin/sh\nexec sleep 10\n", 0755);
  const lx_hotkey_config_t config = {
    .helper_path = script,
    .accelerator = NULL,
    .on_toggle = on_toggle,
    .on_status = NULL,
  };
  lx_hotkey_t* hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  if (hotkey) {
    const long long started = now_ms();
    lx_hotkey_stop(hotkey);
    CHECK(now_ms() - started < 3000);
  }
  unlink(script);
  free(script);
}

/* 5. A named helper that cannot run is refused instead of crashed on. */
static void test_missing_helper(const char* dir)
{
  lx_hotkey_config_t config = {
    .helper_path = "/nonexistent/lx-lyrics-hotkey",
    .accelerator = NULL,
    .on_toggle = on_toggle,
    .on_status = on_status,
  };
  CHECK(lx_hotkey_start(&config) == NULL);

  /* Existing but not executable. */
  char* script = write_script(dir, "not-exec.sh", "#!/bin/sh\nexit 0\n", 0644);
  config.helper_path = script;
  CHECK(lx_hotkey_start(&config) == NULL);
  unlink(script);
  free(script);

  /* NULL config and NULL path. */
  CHECK(lx_hotkey_start(NULL) == NULL);
  config.helper_path = NULL;
  CHECK(lx_hotkey_start(&config) == NULL);
  lx_hotkey_stop(NULL);
}

/* 6. on_status is optional: a chatty helper still toggles and stop is clean. */
static void test_optional_status_callback(const char* dir)
{
  char* script = write_script(dir, "nostatus.sh",
                              "#!/bin/sh\n"
                              "printf 'ready CTRL+ALT+l\\n' >&3\n"
                              "sleep 0.1\n"
                              "printf 'toggle\\n' >&3\n"
                              "sleep 10\n",
                              0755);
  reset_counters();
  const lx_hotkey_config_t config = {
    .helper_path = script,
    .accelerator = NULL,
    .on_toggle = on_toggle,
    .on_status = NULL,
  };
  lx_hotkey_t* hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  if (hotkey) {
    wait_for(&toggles, 1, 5000);
    CHECK(atomic_load(&toggles) == 1);
    lx_hotkey_stop(hotkey);
  }
  unlink(script);
  free(script);
}

int main(void)
{
  char dir[] = "/tmp/lxhotkeytest.XXXXXX";
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    return 2;
  }

  test_spawn_and_delivery(dir);
  test_unavailable(dir);
  test_self_exit_and_idempotent_stop(dir);
  test_mute_helper(dir);
  test_missing_helper(dir);
  test_optional_status_callback(dir);

  rmdir(dir);

  if (failures == 0) {
    printf("hotkey_test: OK\n");
    return 0;
  }
  printf("hotkey_test: %d failure(s)\n", failures);
  return 1;
}
