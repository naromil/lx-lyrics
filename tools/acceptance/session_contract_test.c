/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 *
 * The supervisor's own contract test for plugins/audacious/src/session.{h,c}.
 * Compiled by tools/acceptance/audacious-toggle-gate.py directly against the
 * implementation and the header the brief dictates — it is NOT part of the
 * project's own test targets, so the implementer cannot weaken it.
 *
 * The oracle is a fake spawn/stop pair that counts calls: the state machine
 * must spawn exactly once per transition to shown, stop exactly once per
 * transition to hidden, and never stop what it did not start.
 */
#include "session.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

typedef struct {
  int start_calls;
  int stop_calls;
  int live;
  bool refuse_start;
} fake_t;

static void* fake_start(void* userdata)
{
  fake_t* f = userdata;
  f->start_calls++;
  if (f->refuse_start) {
    return NULL;
  }
  f->live++;
  return (void*)"handle";
}

static void fake_stop(void* handle, void* userdata)
{
  fake_t* f = userdata;
  (void)handle;
  f->stop_calls++;
  f->live--;
}

int main(void)
{
  fake_t fake;
  memset(&fake, 0, sizeof(fake));
  const lx_session_ops_t ops = {.start = fake_start, .stop = fake_stop};

  lx_session_t* s = lx_session_new(&ops, &fake);
  CHECK(s != NULL);
  CHECK(lx_session_showing(s) == false);

  /* show() spawns exactly once; showing again is a no-op. */
  CHECK(lx_session_show(s) == true);
  CHECK(lx_session_showing(s) == true);
  CHECK(fake.start_calls == 1);
  CHECK(lx_session_show(s) == true);
  CHECK(fake.start_calls == 1);

  /* hide() stops exactly once; hiding again is a no-op. */
  lx_session_hide(s);
  CHECK(lx_session_showing(s) == false);
  CHECK(fake.stop_calls == 1);
  lx_session_hide(s);
  CHECK(fake.stop_calls == 1);

  /* toggle alternates. */
  CHECK(lx_session_toggle(s) == true);
  CHECK(lx_session_showing(s) == true);
  CHECK(fake.start_calls == 2);
  CHECK(lx_session_toggle(s) == false);
  CHECK(lx_session_showing(s) == false);
  CHECK(fake.stop_calls == 2);

  /* The app ended on its own (window closed, protocol error, app exited):
   * settle to hidden, remember why, and stay ready to be shown again. */
  CHECK(lx_session_show(s) == true);
  lx_session_app_ended(s, "the lyric window was closed");
  CHECK(lx_session_showing(s) == false);
  CHECK(fake.stop_calls == 3);
  CHECK(lx_session_last_reason(s) != NULL);
  CHECK(strcmp(lx_session_last_reason(s), "the lyric window was closed") == 0);

  /* A refused spawn leaves the session hidden instead of pretending. */
  fake.refuse_start = true;
  CHECK(lx_session_show(s) == false);
  CHECK(lx_session_showing(s) == false);
  CHECK(fake.start_calls == 4);
  CHECK(fake.stop_calls == 3);
  fake.refuse_start = false;

  /* Freeing while shown closes the session (the plugin's cleanup path). */
  CHECK(lx_session_show(s) == true);
  CHECK(fake.live == 1);
  lx_session_free(s);
  CHECK(fake.stop_calls == 4);
  CHECK(fake.live == 0);

  if (failures == 0) {
    printf("session contract: OK (%d starts, %d stops)\n", fake.start_calls, fake.stop_calls);
    return 0;
  }
  printf("session contract: %d failure(s)\n", failures);
  return 1;
}
