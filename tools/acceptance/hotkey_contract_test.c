/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 *
 * The supervisor's own contract test for plugins/audacious/src/hotkey.{h,c}.
 * Compiled by tools/acceptance/audacious-hotkey-gate.py directly against the
 * implementation and the header the brief dictates.
 *
 * The helper here is a /bin/sh stand-in, so the test proves the parts the real
 * binary shares with the adapter: the child gets the pipe on fd 3, its argv
 * carries --pipe-fd and --accelerator, each line is delivered verbatim, and
 * stop() always terminates and reaps a helper (even one that never writes).
 */
#include "hotkey.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
/* Every status line the helper wrote, newline-joined (the argv line included). */
static char history[1024];

static void on_toggle(void* userdata)
{
  (void)userdata;
  atomic_fetch_add(&toggles, 1);
}

static void on_status(const char* line, void* userdata)
{
  (void)userdata;
  const size_t used = strlen(history);
  snprintf(history + used, sizeof(history) - used, "%s\n", line ? line : "");
  atomic_fetch_add(&statuses, 1);
}

static char* write_script(const char* dir, const char* name, const char* body)
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
  chmod(path, 0755);
  return path;
}

static void wait_for(atomic_int* counter, int target, int timeout_ms)
{
  for (int waited = 0; waited < timeout_ms && atomic_load(counter) < target; waited += 10) {
    usleep(10 * 1000);
  }
}

int main(void)
{
  char dir[] = "/tmp/lxhotkeytest.XXXXXX";
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    return 2;
  }

  /* 1. argv + fd 3 contract, two toggles, a clean stop. */
  char* chatty = write_script(dir, "chatty.sh",
                              "#!/bin/sh\n"
                              "printf 'args %s\\n' \"$*\" >&3\n"
                              "printf 'ready CTRL+ALT+l\\n' >&3\n"
                              "sleep 0.2\n"
                              "printf 'toggle\\n' >&3\n"
                              "sleep 0.2\n"
                              "printf 'toggle\\n' >&3\n"
                              "sleep 10\n");
  atomic_store(&toggles, 0);
  atomic_store(&statuses, 0);
  history[0] = '\0';
  lx_hotkey_config_t config = {
    .helper_path = chatty,
    .accelerator = "CTRL+ALT+l",
    .on_toggle = on_toggle,
    .on_status = on_status,
  };
  lx_hotkey_t* hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  wait_for(&statuses, 2, 5000);
  CHECK(atomic_load(&statuses) == 2);
  CHECK(strstr(history, "--pipe-fd 3") != NULL);
  CHECK(strstr(history, "--accelerator CTRL+ALT+l") != NULL);
  CHECK(strstr(history, "ready CTRL+ALT+l") != NULL);
  wait_for(&toggles, 2, 5000);
  CHECK(atomic_load(&toggles) == 2);
  lx_hotkey_stop(hotkey);

  /* 2. A helper that cannot register: its line is delivered, nothing toggles,
   *    and stop() is still clean. */
  char* unavailable = write_script(dir, "unavailable.sh",
                                   "#!/bin/sh\n"
                                   "printf 'unavailable An app id is required\\n' >&3\n"
                                   "sleep 10\n");
  atomic_store(&toggles, 0);
  atomic_store(&statuses, 0);
  history[0] = '\0';
  config.helper_path = unavailable;
  hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  wait_for(&statuses, 1, 5000);
  CHECK(strcmp(history, "unavailable An app id is required\n") == 0);
  CHECK(atomic_load(&toggles) == 0);
  lx_hotkey_stop(hotkey);

  /* 3. A helper that never writes: stop() must still come back, promptly. */
  char* mute = write_script(dir, "mute.sh", "#!/bin/sh\nexec sleep 10\n");
  config.helper_path = mute;
  config.on_status = NULL;
  hotkey = lx_hotkey_start(&config);
  CHECK(hotkey != NULL);
  lx_hotkey_stop(hotkey);

  /* 4. NULL / missing helper: no crash, no session. */
  lx_hotkey_stop(NULL);
  config.helper_path = "/nonexistent/lx-lyrics-hotkey";
  CHECK(lx_hotkey_start(&config) == NULL);

  unlink(chatty);
  unlink(unavailable);
  unlink(mute);
  rmdir(dir);
  free(chatty);
  free(unavailable);
  free(mute);

  if (failures == 0) {
    printf("hotkey contract: OK\n");
    return 0;
  }
  printf("hotkey contract: %d failure(s)\n", failures);
  return 1;
}
