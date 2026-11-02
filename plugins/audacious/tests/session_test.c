/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * Hermetic tests for the "enabled but idle" state machine (src/session.c): a
 * fake start/stop pair counts calls, so every transition is asserted without a
 * player, a display server or the app binary.
 */

#include "session.h"

#include <stdbool.h>
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
  fake_t* fake = userdata;
  fake->start_calls++;
  if (fake->refuse_start) {
    return NULL;
  }
  fake->live++;
  return (void*)"handle";
}

static void fake_stop(void* handle, void* userdata)
{
  fake_t* fake = userdata;
  (void)handle;
  fake->stop_calls++;
  fake->live--;
}

static void test_lifecycle(void)
{
  fake_t fake;
  memset(&fake, 0, sizeof(fake));
  const lx_session_ops_t ops = {.start = fake_start, .stop = fake_stop};

  lx_session_t* session = lx_session_new(&ops, &fake);
  CHECK(session != NULL);
  if (!session) {
    return;
  }
  CHECK(lx_session_showing(session) == false);
  /* Nothing has app-ended yet, so the reason is an empty string, not NULL. */
  CHECK(lx_session_last_reason(session) != NULL);
  CHECK(strcmp(lx_session_last_reason(session), "") == 0);

  /* show() spawns exactly once; showing again is a no-op. */
  CHECK(lx_session_show(session) == true);
  CHECK(lx_session_showing(session) == true);
  CHECK(fake.start_calls == 1);
  CHECK(fake.live == 1);
  CHECK(lx_session_show(session) == true);
  CHECK(fake.start_calls == 1);

  /* hide() stops exactly once; hiding again is a no-op. */
  lx_session_hide(session);
  CHECK(lx_session_showing(session) == false);
  CHECK(fake.stop_calls == 1);
  CHECK(fake.live == 0);
  lx_session_hide(session);
  CHECK(fake.stop_calls == 1);

  /* toggle() alternates and returns the state it settled on. */
  CHECK(lx_session_toggle(session) == true);
  CHECK(lx_session_showing(session) == true);
  CHECK(fake.start_calls == 2);
  CHECK(lx_session_toggle(session) == false);
  CHECK(lx_session_showing(session) == false);
  CHECK(fake.stop_calls == 2);

  /* The app ended on its own: hidden, the reason is recorded, and the state
   * is ready to be shown again. */
  CHECK(lx_session_show(session) == true);
  lx_session_app_ended(session, "the lyric window was closed");
  CHECK(lx_session_showing(session) == false);
  CHECK(fake.stop_calls == 3);
  CHECK(strcmp(lx_session_last_reason(session), "the lyric window was closed") == 0);

  /* app_ended() while already hidden must not stop anything twice. */
  lx_session_app_ended(session, "a later reason");
  CHECK(fake.stop_calls == 3);
  CHECK(strcmp(lx_session_last_reason(session), "a later reason") == 0);

  /* A refused start leaves the session hidden instead of pretending. */
  fake.refuse_start = true;
  CHECK(lx_session_show(session) == false);
  CHECK(lx_session_showing(session) == false);
  CHECK(fake.start_calls == 4);
  CHECK(fake.stop_calls == 3);
  fake.refuse_start = false;

  /* Freeing while shown stops it (the plugin's cleanup path). */
  CHECK(lx_session_show(session) == true);
  CHECK(fake.live == 1);
  lx_session_free(session);
  CHECK(fake.stop_calls == 4);
  CHECK(fake.live == 0);
}

static void test_invalid_ops(void)
{
  fake_t fake;
  memset(&fake, 0, sizeof(fake));
  const lx_session_ops_t incomplete = {.start = fake_start, .stop = NULL};
  CHECK(lx_session_new(NULL, &fake) == NULL);
  CHECK(lx_session_new(&incomplete, &fake) == NULL);

  /* NULL is a tolerated no-op everywhere. */
  CHECK(lx_session_showing(NULL) == false);
  CHECK(lx_session_show(NULL) == false);
  CHECK(lx_session_toggle(NULL) == false);
  lx_session_hide(NULL);
  lx_session_app_ended(NULL, "ignored");
  lx_session_free(NULL);
}

typedef struct {
  fake_t fake;
  int reports;
  bool last_reported;
} reporting_t;

static void* reporting_start(void* userdata)
{
  return fake_start(&((reporting_t*)userdata)->fake);
}

static void reporting_stop(void* handle, void* userdata)
{
  fake_stop(handle, &((reporting_t*)userdata)->fake);
}

static void reporting_changed(bool showing, void* userdata)
{
  reporting_t* reporting = userdata;
  reporting->reports++;
  reporting->last_reported = showing;
}

/* What the plugin persists for the next player start hangs off these reports. */
static void test_showing_changed(void)
{
  reporting_t state;
  memset(&state, 0, sizeof(state));
  const lx_session_ops_t ops = {
    .start = reporting_start, .stop = reporting_stop, .showing_changed = reporting_changed};
  lx_session_t* session = lx_session_new(&ops, &state);
  CHECK(session != NULL);
  if (!session) {
    return;
  }

  /* A refused start changes nothing: an app that is missing today must not
   * rewrite what the user last chose. */
  state.fake.refuse_start = true;
  CHECK(lx_session_show(session) == false);
  CHECK(lx_session_toggle(session) == false);
  CHECK(state.reports == 0);
  state.fake.refuse_start = false;

  CHECK(lx_session_show(session) == true);
  CHECK(state.reports == 1 && state.last_reported == true);
  /* No transition, no report. */
  CHECK(lx_session_show(session) == true);
  CHECK(state.reports == 1);

  CHECK(lx_session_toggle(session) == false);
  CHECK(state.reports == 2 && state.last_reported == false);
  lx_session_hide(session);
  CHECK(state.reports == 2);

  CHECK(lx_session_toggle(session) == true);
  CHECK(state.reports == 3 && state.last_reported == true);

  /* The user closing the window counts as turning it off. */
  lx_session_app_ended(session, "the lyric window was closed");
  CHECK(state.reports == 4 && state.last_reported == false);

  /* Quitting with the window up keeps "showing": free stops it silently. */
  CHECK(lx_session_show(session) == true);
  CHECK(state.reports == 5 && state.last_reported == true);
  lx_session_free(session);
  CHECK(state.fake.live == 0);
  CHECK(state.reports == 5 && state.last_reported == true);
}

static void test_restore_wanted(void)
{
  CHECK(lx_session_restore_wanted(true, true) == true);
  CHECK(lx_session_restore_wanted(true, false) == false);
  CHECK(lx_session_restore_wanted(false, true) == false);
  CHECK(lx_session_restore_wanted(false, false) == false);
}

int main(void)
{
  test_lifecycle();
  test_invalid_ops();
  test_showing_changed();
  test_restore_wanted();

  if (failures == 0) {
    printf("session tests: OK\n");
    return 0;
  }
  printf("session tests: %d failure(s)\n", failures);
  return 1;
}
