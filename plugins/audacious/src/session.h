/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * The adapter's "enabled but idle" state machine: the plugin stays loaded while
 * the lyrics window is hidden, and only an explicit show/toggle starts the app.
 * Pure C with no host headers, so it has its own hermetic test suite
 * (tests/session_test.c) and the supervisor's contract test can compile it.
 */

#ifndef LX_SESSION_H
#define LX_SESSION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lx_session lx_session_t;

typedef struct {
  /* Start the display app. Returns an opaque handle, or NULL when it could not. */
  void* (*start)(void* userdata);
  /* End the session behind @p handle. Called once per successful start(). */
  void (*stop)(void* handle, void* userdata);
} lx_session_ops_t;

lx_session_t* lx_session_new(const lx_session_ops_t* ops, void* userdata);
bool lx_session_showing(const lx_session_t* session);
/* true when a session is running afterwards; a refused start leaves it hidden. */
bool lx_session_show(lx_session_t* session);
void lx_session_hide(lx_session_t* session);
/* Returns the new showing state. */
bool lx_session_toggle(lx_session_t* session);
/* The app ended on its own (window closed / protocol error / app exited). */
void lx_session_app_ended(lx_session_t* session, const char* reason);
const char* lx_session_last_reason(const lx_session_t* session);
void lx_session_free(lx_session_t* session);

#ifdef __cplusplus
}
#endif

#endif /* LX_SESSION_H */
