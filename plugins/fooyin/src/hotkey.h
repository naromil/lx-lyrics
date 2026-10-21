/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * The global-shortcut transport: spawns the lx-lyrics-hotkey helper with the
 * write end of a pipe on fd 3 and reads the helper's one-line reports
 * ("ready ..."/"unavailable ...") and "toggle" presses on a dedicated thread.
 *
 * Pure POSIX C (no GLib, no Audacious headers), so it has its own hermetic test
 * suite (tests/hotkey_test.c) and the supervisor's contract test can compile
 * it. The helper itself lives in lyrics-app/src/hotkey/.
 */

#ifndef LX_HOTKEY_H
#define LX_HOTKEY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lx_hotkey lx_hotkey_t;

/* Delivered on the hotkey reader thread. Do not call lx_hotkey_stop() from here. */
typedef void (*lx_hotkey_toggle_fn)(void* userdata);
/* Delivered on the same thread, once per status line the helper wrote
 * ("ready <trigger>" or "unavailable <reason>"). Optional. */
typedef void (*lx_hotkey_status_fn)(const char* line, void* userdata);

typedef struct {
  const char* helper_path; /* absolute path of the lx-lyrics-hotkey binary */
  const char* accelerator; /* NULL/empty: the helper's own default */
  lx_hotkey_toggle_fn on_toggle;
  lx_hotkey_status_fn on_status;
  void* userdata;
} lx_hotkey_config_t;

/* Spawns the helper with the write end of an internal pipe on fd 3 and reads its
 * lines on a dedicated thread. Returns NULL when the helper cannot be spawned. */
lx_hotkey_t* lx_hotkey_start(const lx_hotkey_config_t* config);

/* Stops the reader, closes the pipe, SIGTERMs the helper and reaps it. Main thread
 * only; NULL is a no-op; idempotent. */
void lx_hotkey_stop(lx_hotkey_t* hotkey);

#ifdef __cplusplus
}
#endif

#endif /* LX_HOTKEY_H */
