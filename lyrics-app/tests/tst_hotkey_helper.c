/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 *
 * Regression test for the helper's Activated handler.
 *
 * org.freedesktop.portal.GlobalShortcuts emits
 *   Activated(o session_handle, s shortcut_id, t timestamp, a{sv} options)
 * and the handler used to parse it as "(ost)". GLib then rejects the format
 * (type mismatch), leaves every out parameter untouched, and the following
 * strcmp(session, app->session_handle) dereferences NULL - the helper died
 * with SIGSEGV on the first key press (coredumpctl: on_activated -> strcmp).
 *
 * The helper is one translation unit with static callbacks, so the test
 * includes it: calling the real on_activated() with the real four-argument
 * payload is the only seam that exercises the bug as it occurs. The helper's
 * main() is renamed so this file can provide the test's main().
 */
#define main lx_hotkey_program_main
// NOLINTNEXTLINE(bugprone-suspicious-include): the helper is one translation unit
#include "../src/hotkey/lx-lyrics-hotkey.c"
#undef main

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_SESSION "/org/freedesktop/portal/desktop/session/1_1/lxlyrics_test"

static int failures;

static void expect_line(const char* name, int read_fd, const char* expected, bool present)
{
  char buffer[256];
  const ssize_t got = read(read_fd, buffer, sizeof(buffer) - 1);
  if (got < 0) {
    if (!present) {
      return;
    }
    fprintf(stderr, "FAIL %s: expected \"%s\", read failed: %s\n", name, expected, strerror(errno));
    failures++;
    return;
  }
  buffer[got] = '\0';
  if (!present) {
    fprintf(stderr, "FAIL %s: expected no line, got \"%s\"\n", name, buffer);
    failures++;
    return;
  }
  if (strcmp(buffer, expected) != 0) {
    fprintf(stderr, "FAIL %s: expected \"%s\", got \"%s\"\n", name, expected, buffer);
    failures++;
    return;
  }
  fprintf(stderr, "ok %s\n", name);
}

/* Builds the portal's Activated payload exactly as the interface defines it. */
static GVariant* activated_payload(const char* session, const char* shortcut_id)
{
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  return g_variant_new("(osta{sv})", session, shortcut_id, (guint64)1234567, &options);
}

int main(void)
{
  int fds[2];
  if (pipe(fds) != 0) {
    perror("pipe");
    return 1;
  }
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);

  LxHotkey app = {
    .pipe_fd = fds[1],
    .loop = NULL, /* never reached: every expected write succeeds */
    .connection = NULL,
    .accelerator = NULL,
    .description = NULL,
    .session_handle = g_strdup(TEST_SESSION),
    .finished = false,
    .timeout_source = 0,
  };

  /* The real signal: the matching session and shortcut id must emit "toggle". */
  GVariant* params = activated_payload(TEST_SESSION, LX_SHORTCUT_ID);
  on_activated(NULL, NULL, NULL, NULL, NULL, params, &app);
  g_variant_unref(params);
  expect_line("portals four-argument Activated", fds[0], "toggle\n", true);

  /* Same shape, foreign session: ignored, no line, no crash. */
  params = activated_payload("/org/freedesktop/portal/desktop/session/9_9/other", LX_SHORTCUT_ID);
  on_activated(NULL, NULL, NULL, NULL, NULL, params, &app);
  g_variant_unref(params);
  expect_line("foreign session", fds[0], "", false);

  /* Same shape, foreign shortcut id: ignored, no line, no crash. */
  params = activated_payload(TEST_SESSION, "another-shortcut");
  on_activated(NULL, NULL, NULL, NULL, NULL, params, &app);
  g_variant_unref(params);
  expect_line("foreign shortcut id", fds[0], "", false);

  close(fds[1]);
  close(fds[0]);
  g_free(app.session_handle);
  return failures == 0 ? 0 : 1;
}
