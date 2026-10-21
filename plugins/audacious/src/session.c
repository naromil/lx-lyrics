/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * The adapter's "enabled but idle" state machine (see session.h).
 *
 * Not thread-safe by design: plugin.cc drives it from the host's main thread
 * only, where spawning and stopping the app already live.
 */

#include "session.h"

#include <stdio.h>
#include <stdlib.h>

/** Longest last-app-ended reason kept (the plugin's own reasons are short). */
#define LX_SESSION_REASON_SIZE 160

struct lx_session {
  lx_session_ops_t ops;
  void* userdata;
  /* The value start() returned; handed back to stop() exactly once. */
  void* handle;
  bool showing;
  /* "" until app_ended() records something. */
  char last_reason[LX_SESSION_REASON_SIZE];
};

lx_session_t* lx_session_new(const lx_session_ops_t* ops, void* userdata)
{
  if (!ops || !ops->start || !ops->stop) {
    return NULL;
  }
  lx_session_t* session = calloc(1, sizeof(*session));
  if (!session) {
    return NULL;
  }
  session->ops = *ops;
  session->userdata = userdata;
  session->last_reason[0] = '\0';
  return session;
}

bool lx_session_showing(const lx_session_t* session)
{
  return session && session->showing;
}

bool lx_session_show(lx_session_t* session)
{
  if (!session) {
    return false;
  }
  if (session->showing) {
    return true;
  }
  void* handle = session->ops.start(session->userdata);
  if (!handle) {
    /* A refused start leaves the state hidden; only a successful start()
     * produces a handle that stop() may ever see. */
    return false;
  }
  session->handle = handle;
  session->showing = true;
  return true;
}

void lx_session_hide(lx_session_t* session)
{
  if (!session || !session->showing) {
    return;
  }
  session->ops.stop(session->handle, session->userdata);
  session->handle = NULL;
  session->showing = false;
}

bool lx_session_toggle(lx_session_t* session)
{
  if (!session) {
    return false;
  }
  if (session->showing) {
    lx_session_hide(session);
    return false;
  }
  return lx_session_show(session);
}

void lx_session_app_ended(lx_session_t* session, const char* reason)
{
  if (!session) {
    return;
  }
  snprintf(session->last_reason, sizeof(session->last_reason), "%s",
           reason ? reason : "the session ended");
  /* The app is already gone; stop() only joins/reaps what start() created. */
  lx_session_hide(session);
}

const char* lx_session_last_reason(const lx_session_t* session)
{
  return session ? session->last_reason : NULL;
}

void lx_session_free(lx_session_t* session)
{
  if (!session) {
    return;
  }
  /* The plugin's cleanup path: a session still showing must be stopped. */
  lx_session_hide(session);
  free(session);
}
