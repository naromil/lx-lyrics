/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 *
 * lx-lyrics-hotkey - the adapter-side global-shortcut helper.
 *
 * The Audacious plugin spawns this binary with the write end of a pipe on
 * --pipe-fd and reads one newline-terminated line per event:
 *
 *   ready <trigger_description>   the portal bound the shortcut; keep running
 *   unavailable <reason>          the binding failed; exit 0
 *   toggle                        one per Activated signal
 *
 * The key itself comes from the XDG desktop portal
 * (org.freedesktop.portal.GlobalShortcuts). libaudcore has no hotkey API, and
 * keeping the portal client out of the plugin means its crash domain and its
 * GLib main loop stay separate from the player's.
 *
 * The portal derives the application id from the systemd user unit name
 * ("app-<id>@<uuid>.service"). A player started from a desktop launcher has
 * one; a player started from a terminal has none, and the portal refuses with
 * org.freedesktop.portal.Error.NotAllowed ("An app id is required"). That is a
 * normal outcome, reported as "unavailable" on the pipe - never an error exit.
 *
 * Every D-Bus call is asynchronous: a synchronous call from a signal callback
 * would block the very main context that must dispatch its reply.
 */

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LX_PORTAL_BUS "org.freedesktop.portal.Desktop"
#define LX_PORTAL_PATH "/org/freedesktop/portal/desktop"
#define LX_PORTAL_SHORTCUTS "org.freedesktop.portal.GlobalShortcuts"
#define LX_PORTAL_REQUEST "org.freedesktop.portal.Request"
#define LX_PORTAL_SESSION "org.freedesktop.portal.Session"
#define LX_SHORTCUT_ID "toggle"
#define LX_DEFAULT_DESCRIPTION "Toggle desktop lyrics"
#define LX_TOKEN_SIZE 64
#define LX_TRIGGER_SIZE 256
#define LX_REASON_SIZE 1024
/* How long the desktop gets to answer a portal request before the helper gives
 * up. Measured (xdg-desktop-portal-kde 6.8): when the preferred trigger is
 * already taken, the backend fails to build its own "shortcut is already in
 * use" message (journal: `0 instead of 1 arguments to message ...`) and NEVER
 * sends the Response signal, so without this the helper waits forever and the
 * plugin hears nothing — not even "unavailable". */
#define LX_PORTAL_TIMEOUT_S 10

typedef struct {
  int pipe_fd;
  GMainLoop* loop;
  GDBusConnection* connection;
  char* accelerator;
  char* description;
  char session_token[LX_TOKEN_SIZE];
  char create_token[LX_TOKEN_SIZE];
  char bind_token[LX_TOKEN_SIZE];
  char* session_handle;
  /* Set once an outcome was reported; later failures are ignored. */
  bool finished;
  /* The pending portal request's watchdog (see LX_PORTAL_TIMEOUT_S). */
  guint timeout_source;
} LxHotkey;

/* ---------------------------------------------------------------- the pipe */

/* Writes one line to the plugin. Returns false when the pipe is gone (the
 * player closed it, which is the quit trigger). */
static bool emit_line(LxHotkey* app, const char* format, ...)
{
  char line[LX_REASON_SIZE];
  va_list args;
  va_start(args, format);
  int used = vsnprintf(line, sizeof(line) - 2, format, args);
  va_end(args);
  if (used < 0) {
    return false;
  }
  if ((size_t)used > sizeof(line) - 2) {
    used = (int)(sizeof(line) - 2);
  }
  line[used++] = '\n';
  line[used] = '\0';

  ssize_t written = write(app->pipe_fd, line, (size_t)used);
  return written == (ssize_t)used || (written < 0 && errno == EINTR);
}

/* Reports the single terminal failure line and leaves the loop. */
static void report_unavailable(LxHotkey* app, const char* reason)
{
  if (app->finished) {
    return;
  }
  app->finished = true;
  if (app->timeout_source) {
    g_source_remove(app->timeout_source);
    app->timeout_source = 0;
  }
  emit_line(app, "unavailable %s", reason && reason[0] ? reason : "the shortcut is unavailable");
  g_main_loop_quit(app->loop);
}

/* The watchdog for a portal request that never answers (see LX_PORTAL_TIMEOUT_S). */
static gboolean on_portal_timeout(gpointer userdata)
{
  LxHotkey* app = userdata;
  app->timeout_source = 0;
  report_unavailable(app, "the desktop did not answer the shortcut request");
  return G_SOURCE_REMOVE;
}

static void arm_portal_timeout(LxHotkey* app)
{
  if (app->timeout_source) {
    g_source_remove(app->timeout_source);
  }
  app->timeout_source = g_timeout_add_seconds(LX_PORTAL_TIMEOUT_S, on_portal_timeout, app);
}

static void cancel_portal_timeout(LxHotkey* app)
{
  if (app->timeout_source) {
    g_source_remove(app->timeout_source);
    app->timeout_source = 0;
  }
}

/* A portal/GDBus error, with the remote error name and message preserved (the
 * no-app-id case is the portal's own "An app id is required"). */
static void report_unavailable_error(LxHotkey* app, const char* prefix, const GError* error)
{
  char reason[LX_REASON_SIZE];
  if (error) {
    GError* copy = g_error_copy(error);
    g_dbus_error_strip_remote_error(copy);
    if (prefix && prefix[0]) {
      snprintf(reason, sizeof(reason), "%s: %s", prefix, copy->message);
    } else {
      snprintf(reason, sizeof(reason), "%s", copy->message);
    }
    g_error_free(copy);
  } else {
    snprintf(reason, sizeof(reason), "%s", prefix ? prefix : "the shortcut is unavailable");
  }
  report_unavailable(app, reason);
}

/* ------------------------------------------------------ portal request ids */

static gboolean on_stdin_gone(gint fd, GIOCondition condition, gpointer userdata)
{
  (void)fd;
  (void)condition;
  LxHotkey* app = userdata;
  g_main_loop_quit(app->loop);
  return G_SOURCE_REMOVE;
}

static void make_token(char* token, size_t size, const char* kind)
{
  snprintf(token, size, "lxlyrics_%s_%u_%u", kind, (unsigned)getpid(), g_random_int());
}

/* The portal predicts the request object path from the caller's unique bus
 * name and the handle_token:
 * /org/freedesktop/portal/desktop/request/<sender>/<handle_token>
 * Subscribing there before the call avoids the reply-before-subscribe race. */
static char* predict_request_path(LxHotkey* app, const char* token)
{
  const char* unique = g_dbus_connection_get_unique_name(app->connection);
  char* sender = g_strdup(unique && unique[0] == ':' ? unique + 1 : unique);
  for (char* cursor = sender; cursor && *cursor; cursor++) {
    if (*cursor == '.') {
      *cursor = '_';
    }
  }
  char* path = g_strdup_printf("%s/request/%s/%s", LX_PORTAL_PATH, sender ? sender : "", token);
  g_free(sender);
  return path;
}

static guint subscribe_response(LxHotkey* app, const char* path, GDBusSignalCallback callback)
{
  return g_dbus_connection_signal_subscribe(app->connection, LX_PORTAL_BUS, LX_PORTAL_REQUEST,
                                            "Response", path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                            callback, app, NULL);
}

/* ------------------------------------------------------- signal callbacks */

static void on_activated(GDBusConnection* connection, const char* sender_name,
                         const char* object_path, const char* interface_name,
                         const char* signal_name, GVariant* parameters, gpointer userdata)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  LxHotkey* app = userdata;

  const char* session = NULL;
  const char* shortcut_id = NULL;
  guint64 timestamp = 0;
  g_variant_get(parameters, "(&o&st)", &session, &shortcut_id, &timestamp);
  (void)timestamp;
  if (app->session_handle && strcmp(session, app->session_handle) != 0) {
    return;
  }
  if (shortcut_id && strcmp(shortcut_id, LX_SHORTCUT_ID) != 0) {
    return;
  }
  if (!emit_line(app, "toggle")) {
    g_main_loop_quit(app->loop);
  }
}

static void on_session_closed(GDBusConnection* connection, const char* sender_name,
                              const char* object_path, const char* interface_name,
                              const char* signal_name, GVariant* parameters, gpointer userdata)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  (void)parameters;
  LxHotkey* app = userdata;
  /* The desktop closed the session: the shortcut is gone, keep the exit code
   * clean and let the plugin keep working without one. */
  g_main_loop_quit(app->loop);
}

static gboolean on_terminate(gpointer userdata)
{
  LxHotkey* app = userdata;
  g_main_loop_quit(app->loop);
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------- portal: binding */

static void bind_shortcuts(LxHotkey* app);

static void on_bind_called(GObject* source, GAsyncResult* result, gpointer userdata)
{
  (void)source;
  LxHotkey* app = userdata;
  GError* error = NULL;
  GVariant* reply =
    g_dbus_connection_call_finish(G_DBUS_CONNECTION(app->connection), result, &error);
  if (!reply) {
    report_unavailable_error(app, "the desktop refused to bind the shortcut", error);
    g_clear_error(&error);
    return;
  }
  g_variant_unref(reply);
}

static void on_bind_response(GDBusConnection* connection, const char* sender_name,
                             const char* object_path, const char* interface_name,
                             const char* signal_name, GVariant* parameters, gpointer userdata)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  LxHotkey* app = userdata;
  cancel_portal_timeout(app);

  guint32 response = 2;
  GVariant* results = NULL;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  if (response != 0) {
    g_variant_unref(results);
    report_unavailable(app, response == 1 ? "the shortcut request was cancelled"
                                          : "the desktop refused the shortcut request");
    return;
  }

  /* The trigger the desktop actually assigned; it may differ from the
   * preferred one when that combination was already taken. */
  char trigger[LX_TRIGGER_SIZE] = "";
  GVariant* shortcuts = g_variant_lookup_value(results, "shortcuts", G_VARIANT_TYPE("a(sa{sv})"));
  if (shortcuts) {
    GVariantIter iter;
    const char* id = NULL;
    GVariant* options = NULL;
    g_variant_iter_init(&iter, shortcuts);
    if (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &options)) {
      GVariant* description =
        g_variant_lookup_value(options, "trigger_description", G_VARIANT_TYPE_STRING);
      if (description) {
        snprintf(trigger, sizeof(trigger), "%s", g_variant_get_string(description, NULL));
        g_variant_unref(description);
      }
      g_variant_unref(options);
    }
    g_variant_unref(shortcuts);
  }
  g_variant_unref(results);

  if (trigger[0] == '\0') {
    snprintf(trigger, sizeof(trigger), "%s",
             app->accelerator && app->accelerator[0] ? app->accelerator
                                                     : "assigned by the desktop");
  }
  emit_line(app, "ready %s", trigger);
}

static void bind_shortcuts(LxHotkey* app)
{
  make_token(app->bind_token, sizeof(app->bind_token), "bind");

  GVariantBuilder shortcut_options;
  g_variant_builder_init(&shortcut_options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&shortcut_options, "{sv}", "description",
                        g_variant_new_string(app->description));
  if (app->accelerator && app->accelerator[0]) {
    g_variant_builder_add(&shortcut_options, "{sv}", "preferred_trigger",
                          g_variant_new_string(app->accelerator));
  }

  GVariantBuilder shortcuts;
  g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
  g_variant_builder_add(&shortcuts, "(sa{sv})", LX_SHORTCUT_ID, &shortcut_options);

  GVariantBuilder call_options;
  g_variant_builder_init(&call_options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&call_options, "{sv}", "handle_token",
                        g_variant_new_string(app->bind_token));

  char* request_path = predict_request_path(app, app->bind_token);
  subscribe_response(app, request_path, on_bind_response);
  g_free(request_path);

  arm_portal_timeout(app);
  g_dbus_connection_call(
    app->connection, LX_PORTAL_BUS, LX_PORTAL_PATH, LX_PORTAL_SHORTCUTS, "BindShortcuts",
    g_variant_new("(o@a(sa{sv})s@a{sv})", app->session_handle, g_variant_builder_end(&shortcuts),
                  "", g_variant_builder_end(&call_options)),
    G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_bind_called, app);
}

/* --------------------------------------------------- portal: CreateSession */

static void on_create_response(GDBusConnection* connection, const char* sender_name,
                               const char* object_path, const char* interface_name,
                               const char* signal_name, GVariant* parameters, gpointer userdata)
{
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  LxHotkey* app = userdata;
  cancel_portal_timeout(app);

  guint32 response = 2;
  GVariant* results = NULL;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);
  if (response != 0) {
    g_variant_unref(results);
    report_unavailable(app, response == 1 ? "the shortcut request was cancelled"
                                          : "the desktop refused the shortcut request");
    return;
  }

  GVariant* session = g_variant_lookup_value(results, "session_handle", G_VARIANT_TYPE_STRING);
  g_variant_unref(results);
  if (!session) {
    report_unavailable(app, "the desktop returned no session handle");
    return;
  }
  app->session_handle = g_variant_dup_string(session, NULL);
  g_variant_unref(session);

  g_dbus_connection_signal_subscribe(app->connection, LX_PORTAL_BUS, LX_PORTAL_SESSION, "Closed",
                                     app->session_handle, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                     on_session_closed, app, NULL);
  bind_shortcuts(app);
}

static void on_create_called(GObject* source, GAsyncResult* result, gpointer userdata)
{
  (void)source;
  LxHotkey* app = userdata;
  GError* error = NULL;
  GVariant* reply =
    g_dbus_connection_call_finish(G_DBUS_CONNECTION(app->connection), result, &error);
  if (!reply) {
    /* The no-app-id case lands here: the portal's NotAllowed error. */
    report_unavailable_error(app, NULL, error);
    g_clear_error(&error);
    return;
  }
  g_variant_unref(reply);
}

static void create_session(LxHotkey* app)
{
  make_token(app->create_token, sizeof(app->create_token), "create");
  make_token(app->session_token, sizeof(app->session_token), "session");

  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(app->create_token));
  g_variant_builder_add(&options, "{sv}", "session_handle_token",
                        g_variant_new_string(app->session_token));

  char* request_path = predict_request_path(app, app->create_token);
  subscribe_response(app, request_path, on_create_response);
  g_free(request_path);

  arm_portal_timeout(app);
  g_dbus_connection_call(app->connection, LX_PORTAL_BUS, LX_PORTAL_PATH, LX_PORTAL_SHORTCUTS,
                         "CreateSession", g_variant_new("(a{sv})", &options), G_VARIANT_TYPE("(o)"),
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_create_called, app);
}

/* ------------------------------------------------------------- connection */

static void on_bus(GObject* source, GAsyncResult* result, gpointer userdata)
{
  (void)source;
  LxHotkey* app = userdata;
  GError* error = NULL;
  GDBusConnection* connection = g_bus_get_finish(result, &error);
  if (!connection) {
    report_unavailable_error(app, "no session bus", error);
    g_clear_error(&error);
    return;
  }
  app->connection = connection;

  /* Activated is emitted on the portal object, not on the session object. */
  g_dbus_connection_signal_subscribe(connection, LX_PORTAL_BUS, LX_PORTAL_SHORTCUTS, "Activated",
                                     LX_PORTAL_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_activated,
                                     app, NULL);
  create_session(app);
}

/* ------------------------------------------------------------------ main */

static void usage(FILE* stream)
{
  fprintf(stream, "usage: lx-lyrics-hotkey --pipe-fd N [--accelerator STR] [--description STR]\n"
                  "\n"
                  "  --pipe-fd N        write the helper's event lines to descriptor N (required)\n"
                  "  --accelerator STR  preferred trigger, e.g. CTRL+ALT+l\n"
                  "  --description STR  shortcut description shown by the desktop\n");
}

int main(int argc, char** argv)
{
  /* A closed pipe is the player going away, not a fatal condition. */
  signal(SIGPIPE, SIG_IGN);

  int pipe_fd = -1;
  const char* accelerator = NULL;
  const char* description = LX_DEFAULT_DESCRIPTION;

  for (int i = 1; i < argc; i++) {
    const char* arg = argv[i];
    const char* value = NULL;
    const char* equals = strchr(arg, '=');
    const size_t name_len = equals ? (size_t)(equals - arg) : strlen(arg);

    if (name_len == 9 && strncmp(arg, "--pipe-fd", 9) == 0) {
      if (equals) {
        value = equals + 1;
      } else if (i + 1 < argc) {
        value = argv[++i];
      }
      if (!value) {
        usage(stderr);
        return 2;
      }
      char* end = NULL;
      errno = 0;
      const long parsed = strtol(value, &end, 10);
      if (errno != 0 || end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        usage(stderr);
        return 2;
      }
      pipe_fd = (int)parsed;
      continue;
    }
    if (name_len == 13 && strncmp(arg, "--accelerator", 13) == 0) {
      if (equals) {
        value = equals + 1;
      } else if (i + 1 < argc) {
        value = argv[++i];
      }
      if (!value) {
        usage(stderr);
        return 2;
      }
      accelerator = value;
      continue;
    }
    if (name_len == 13 && strncmp(arg, "--description", 13) == 0) {
      if (equals) {
        value = equals + 1;
      } else if (i + 1 < argc) {
        value = argv[++i];
      }
      if (!value) {
        usage(stderr);
        return 2;
      }
      description = value;
      continue;
    }
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      usage(stdout);
      return 0;
    }
    fprintf(stderr, "lx-lyrics-hotkey: unknown argument '%s'\n", arg);
    usage(stderr);
    return 2;
  }

  if (pipe_fd < 0 || fcntl(pipe_fd, F_GETFD) < 0) {
    fprintf(stderr, "lx-lyrics-hotkey: --pipe-fd is required and must be an open descriptor\n");
    usage(stderr);
    return 2;
  }

  LxHotkey app = {
    .pipe_fd = pipe_fd,
    .loop = g_main_loop_new(NULL, FALSE),
    .connection = NULL,
    .accelerator = g_strdup(accelerator ? accelerator : ""),
    .description = g_strdup(description && description[0] ? description : LX_DEFAULT_DESCRIPTION),
    .session_handle = NULL,
    .finished = false,
    .timeout_source = 0,
  };

  g_unix_signal_add(SIGTERM, on_terminate, &app);
  g_unix_signal_add(SIGINT, on_terminate, &app);

  /* If the plugin's read end closes, poll(2) tells us even when the helper has
   * no D-Bus traffic; the player is gone and the helper must follow. */
  GSource* pipe_source = g_unix_fd_source_new(pipe_fd, G_IO_ERR | G_IO_HUP);
  g_source_set_callback(pipe_source, G_SOURCE_FUNC(on_stdin_gone), &app, NULL);
  g_source_attach(pipe_source, NULL);

  g_bus_get(G_BUS_TYPE_SESSION, NULL, on_bus, &app);
  g_main_loop_run(app.loop);
  g_main_loop_unref(app.loop);
  g_free(app.accelerator);
  g_free(app.description);
  g_free(app.session_handle);
  if (app.connection) {
    g_object_unref(app.connection);
  }
  return 0;
}
