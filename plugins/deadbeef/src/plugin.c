/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * DeaDBeeF adapter for the standalone lx-lyrics display: the plugin lives in the
 * player process, spawns the app as its direct child (feed.c) and pushes the
 * protocol v2 player feed described in docs/protocol.md. It reads no lyrics -
 * the app owns acquisition, parsing and rendering.
 */

/*
 * Strict -std=c99 (the README's one-line build) hides the POSIX declarations
 * used here: PATH_MAX, getenv(), access().
 */
#define _POSIX_C_SOURCE 200809L

/*
 * The plugin compiles against DeaDBeeF's API 1.16 and declares the same floor in
 * the plugin struct the loader checks, so using newer API is a compile error.
 * 1.16 is the floor because streamer_get_playing_track_safe() (the only
 * race-free "current track" accessor, added in 1.16) is the newest call used
 * here; vis_spectrum_listen2() is 1.15. Never DDB_PLUGIN_SET_API_VERSION, which
 * would pin the build to the headers' own version.
 */
#define DDB_API_LEVEL 16

#include <deadbeef/deadbeef.h>

#include "feed.h"
#include "spectrum.h"

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Config key holding the desktop-lyrics on/off state (the toggle has no checkable flag). */
#define LX_CONF_ENABLED "lxlyrics.enabled"
/** Optional config key overriding where lx-lyrics-app lives. */
#define LX_CONF_APP_PATH "lxlyrics.app_path"
/** Name reported in `hello.host`. */
#define LX_HOST_NAME "deadbeef"
/** The display app, resolved on $PATH when LX_CONF_APP_PATH is empty. */
#define LX_APP_NAME "lx-lyrics-app"
/** Metadata values are copied out of the playlist lock into buffers this big. */
#define LX_META_SIZE 1024

/*
 * Plugin-private message ids. DeaDBeeF reserves 1..28 and the 1000..1007
 * structured range. The feed thread uses these to hand its notifications over
 * instead of touching the session itself: message() is dispatched on
 * DeaDBeeF's player thread, never on the feed thread.
 */
#define LX_EV_FEED_CLOSE_REQUESTED 0x4c590001u
#define LX_EV_FEED_CHILD_EXITED 0x4c590002u
#define LX_EV_FEED_PROTOCOL_ERROR 0x4c590003u

static DB_functions_t* deadbeef;

/*
 * The running session.
 *
 * Two threads mutate these pointers: the GTK main thread (the View/LX Lyrics
 * action callback — upstream plugins/gtkui/actions.c runs
 * `gdk_threads_add_idle (menu_action_cb, action)` -> `action->callback2 (action,
 * DDB_ACTION_CTX_MAIN)`) and DeaDBeeF's player thread (upstream src/main.c:
 * `mainloop_tid = thread_start (mainloop_thread, NULL)`, whose player_mainloop()
 * broadcasts `plugs[n]->message (msg, ctx, p1, p2)`). Neither is "the main
 * thread", and they run concurrently.
 *
 * Both pointers are therefore published and cleared in ONE step under
 * session_lock (start_session/stop_session), and a pointer read here is only a
 * borrow: session_feed_acquire() takes a feed reference while the lock is held
 * and the caller releases it, so the other thread may end and release the
 * session without freeing a feed still in use (see feed.h). The spectrum
 * pointer is only ever used under session_lock.
 */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static lx_feed_t* session_feed;
static lx_spectrum_t* session_spectrum;

/* Borrows the session's feed: non-NULL only while a session runs, and alive
 * until the matching lx_feed_release(). */
static lx_feed_t* session_feed_acquire(void)
{
  pthread_mutex_lock(&session_lock);
  lx_feed_t* feed = lx_feed_acquire(session_feed);
  pthread_mutex_unlock(&session_lock);
  return feed;
}

static int64_t current_position_ms(void)
{
  return (int64_t)(deadbeef->streamer_get_playpos() * 1000.0f);
}

static ddb_playback_state_t current_state(void)
{
  DB_output_t* output = deadbeef->get_output();
  return output ? output->state() : DDB_PLAYBACK_STATE_STOPPED;
}

/* Copies a metadata value out of the playlist lock: pl_find_meta's pointer is
 * only valid while pl_lock() is held. */
static void copy_meta(DB_playItem_t* track, const char* key, char* value, size_t value_size)
{
  const char* meta = deadbeef->pl_find_meta(track, key);
  if (meta) {
    snprintf(value, value_size, "%s", meta);
  } else {
    value[0] = '\0';
  }
}

/* set_info for @p track: `path` is the local file the app reads lyrics from.
 * @p feed is borrowed by the caller. */
static void push_info(lx_feed_t* feed, DB_playItem_t* track)
{
  if (!feed || !track) {
    return;
  }
  char uri[PATH_MAX];
  char singer[LX_META_SIZE];
  char name[LX_META_SIZE];
  char album[LX_META_SIZE];
  deadbeef->pl_lock();
  copy_meta(track, ":URI", uri, sizeof(uri));
  copy_meta(track, "artist", singer, sizeof(singer));
  copy_meta(track, "title", name, sizeof(name));
  copy_meta(track, "album", album, sizeof(album));
  deadbeef->pl_unlock();

  const char* path = uri;
  if (strncmp(path, "file://", 7) == 0) {
    path += 7;
  }
  lx_feed_send_info(feed, path, singer, name, album, current_state() == DDB_PLAYBACK_STATE_PLAYING,
                    current_position_ms());
}

/* The app (re)starts the lyric timer from set_play, set_pause or set_stop.
 * @p feed is borrowed by the caller. */
static void push_state(lx_feed_t* feed)
{
  if (!feed) {
    return;
  }
  switch (current_state()) {
  case DDB_PLAYBACK_STATE_PLAYING:
    lx_feed_send_play(feed, current_position_ms());
    break;
  case DDB_PLAYBACK_STATE_PAUSED:
    lx_feed_send_pause(feed);
    break;
  default:
    lx_feed_send_stop(feed);
    break;
  }
}

static void on_analyser_data_requested(lx_feed_t* feed, void* userdata)
{
  (void)userdata;
  uint8_t frame[LX_FEED_SPECTRUM_BYTES];
  pthread_mutex_lock(&session_lock);
  lx_spectrum_t* spectrum = session_spectrum;
  bool ready = spectrum && lx_spectrum_read(spectrum, frame, sizeof(frame));
  pthread_mutex_unlock(&session_lock);
  if (ready) {
    lx_feed_send_spectrum(feed, frame, sizeof(frame));
  }
}

static void on_close_requested(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  (void)userdata;
  deadbeef->sendmessage(LX_EV_FEED_CLOSE_REQUESTED, 0, 0, 0);
}

/* The app violated the protocol (§7): the feed already stopped the session by
 * closing the app's stdin, so the host only has to end its own bookkeeping —
 * no respawn, and the toggle goes off. */
static void on_protocol_error(lx_feed_t* feed, void* userdata, const char* reason)
{
  (void)feed;
  (void)userdata;
  deadbeef->log("lxlyrics: the app violated the protocol (%s)", reason ? reason : "unknown");
  deadbeef->sendmessage(LX_EV_FEED_PROTOCOL_ERROR, 0, 0, 0);
}

static void on_child_exited(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  (void)userdata;
  deadbeef->sendmessage(LX_EV_FEED_CHILD_EXITED, 0, 0, 0);
}

/* Runs on the feed thread every LX_FEED_STATUS_INTERVAL_MS: the periodic
 * set_status that keeps the app's line in sync while the track plays. */
static bool on_sample_status(lx_feed_t* feed, void* userdata, bool* is_play,
                             int64_t* played_time_ms)
{
  (void)feed;
  (void)userdata;
  if (current_state() != DDB_PLAYBACK_STATE_PLAYING) {
    return false;
  }
  *is_play = true;
  *played_time_ms = current_position_ms();
  return true;
}

/* Releases the running session, if any. Callable from either session thread:
 * the swap of both pointers is one locked step, so a concurrent
 * start_session() either publishes a whole session before this or after it.
 * lx_feed_stop() joins the reader thread and drops the session's reference; a
 * feed a message handler still holds stays alive until that handler releases
 * it. */
static void stop_session(void)
{
  pthread_mutex_lock(&session_lock);
  lx_feed_t* feed = session_feed;
  lx_spectrum_t* spectrum = session_spectrum;
  session_feed = NULL;
  session_spectrum = NULL;
  pthread_mutex_unlock(&session_lock);

  /* Unlisten before the child is torn down so no analyser frame can race the
   * feed's release. */
  lx_spectrum_stop(spectrum);
  lx_feed_stop(feed);
}

/* Whether a session is published right now. Cheap enough for the config-change
 * path, which runs on every key the host rewrites. */
static bool session_running(void)
{
  pthread_mutex_lock(&session_lock);
  const bool running = session_feed != NULL;
  pthread_mutex_unlock(&session_lock);
  return running;
}

/* The session ended on its own: release it and clear the toggle state, so the
 * menu item honestly reads off and a fresh one can be started next time. */
static void end_session(const char* reason)
{
  if (session_running()) {
    deadbeef->log("lxlyrics: ending the session (%s)", reason);
  }
  stop_session();
  if (deadbeef->conf_get_int(LX_CONF_ENABLED, 0) != 0) {
    deadbeef->conf_set_int(LX_CONF_ENABLED, 0);
    /* The action API has no checkable flag, so the conf key IS the item's
     * state: this rebuild tells the UI to re-read it. */
    deadbeef->sendmessage(DB_EV_ACTIONSCHANGED, 0, 0, 0);
  }
}

/* $LX_CONF_APP_PATH when set, otherwise lx-lyrics-app on $PATH. */
static bool resolve_app_path(char* path, size_t path_size)
{
  deadbeef->conf_get_str(LX_CONF_APP_PATH, "", path, (int)path_size);
  if (path[0] != '\0') {
    return true;
  }
  const char* search = getenv("PATH");
  if (!search) {
    return false;
  }
  for (const char* dir = search; *dir;) {
    const char* end = strchr(dir, ':');
    size_t dir_len = end ? (size_t)(end - dir) : strlen(dir);
    if (dir_len > 0 && dir_len + 1 + sizeof(LX_APP_NAME) <= path_size) {
      memcpy(path, dir, dir_len);
      path[dir_len] = '/';
      memcpy(path + dir_len + 1, LX_APP_NAME, sizeof(LX_APP_NAME));
      if (access(path, X_OK) == 0) {
        return true;
      }
    }
    if (!end) {
      break;
    }
    dir = end + 1;
  }
  path[0] = '\0';
  return false;
}

/* Starts the session when the wanted state says so, and sets the wanted state
 * itself when the child is up (the single place the key is turned on). One
 * session at a time: the GTK toggle and the start-up restore can ask at the
 * same moment, so the whole start is serialized, and both session pointers are
 * published in a single locked step — a concurrent end_session() can neither
 * stop a half-built session nor leave a spawned child unowned. */
static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;

static bool start_session(void)
{
  char app_path[PATH_MAX];
  if (!resolve_app_path(app_path, sizeof(app_path))) {
    deadbeef->log("lxlyrics: %s not found; install it or set the \"%s\" config key", LX_APP_NAME,
                  LX_CONF_APP_PATH);
    return false;
  }

  pthread_mutex_lock(&start_lock);
  pthread_mutex_lock(&session_lock);
  const bool already_running = session_feed != NULL;
  pthread_mutex_unlock(&session_lock);

  bool started = already_running;
  if (already_running) {
    /* The session the caller wants is already up: adopt it, so the conf key,
     * the menu item and the child agree even if the key was cleared
     * out-of-band. Never a second child. */
    deadbeef->log("lxlyrics: a session is already running; keeping it");
  } else {
    /* Subscribe before spawning: the app may ask for a frame as soon as it has
     * read the hello line, and hello.spectrum is only true when this succeeds. */
    lx_spectrum_t* spectrum = lx_spectrum_start(deadbeef);

    const lx_feed_callbacks_t callbacks = {
      .analyser_data_requested = on_analyser_data_requested,
      .close_requested = on_close_requested,
      .protocol_error = on_protocol_error,
      .child_exited = on_child_exited,
      .sample_status = on_sample_status,
    };
    lx_feed_t* feed = lx_feed_spawn(app_path, LX_HOST_NAME, spectrum != NULL, &callbacks, NULL);
    if (!feed) {
      lx_spectrum_stop(spectrum);
      deadbeef->log("lxlyrics: cannot spawn %s", app_path);
    } else {
      /* One step: either the whole session is visible or none of it is. A frame
       * request that races this publish is simply unanswered — the app asks
       * again for the next frame. */
      pthread_mutex_lock(&session_lock);
      session_spectrum = spectrum;
      session_feed = feed;
      pthread_mutex_unlock(&session_lock);

      deadbeef->log("lxlyrics: session started (%s, spectrum %s)", app_path,
                    spectrum ? "on" : "unavailable");

      /* The host pushes the initial snapshot right after spawning (protocol.md
       * §3), so the app renders what is already playing. */
      DB_playItem_t* track = deadbeef->streamer_get_playing_track_safe();
      if (track) {
        push_info(feed, track);
        deadbeef->pl_item_unref(track);
      }
      push_state(feed);
      started = true;
    }
  }

  if (started && deadbeef->conf_get_int(LX_CONF_ENABLED, 0) == 0) {
    deadbeef->conf_set_int(LX_CONF_ENABLED, 1);
    /* The action API has no checkable flag, so the conf key IS the item's
     * state: this rebuild tells the UI to re-read it. */
    deadbeef->sendmessage(DB_EV_ACTIONSCHANGED, 0, 0, 0);
  }
  pthread_mutex_unlock(&start_lock);
  return started;
}

/* The persisted wanted state is the contract this adapter documents ("the
 * toggle state lives in lxlyrics.enabled"), so a new player process restores
 * it. DB_EV_PLUGINSLOADED is the first message after every plugin is loaded and
 * the streamer is up (upstream src/main.c: `streamer_init (); plug_connect_all
 * (); messagepump_push (DB_EV_PLUGINSLOADED, 0, 0, 0);`), which is exactly when
 * a session can be started — and it runs on the player thread, so the spawn
 * does not need the GTK side. */
static bool restore_attempted;

static void restore_session(void)
{
  if (restore_attempted) {
    return;
  }
  restore_attempted = true;

  if (deadbeef->conf_get_int(LX_CONF_ENABLED, 0) == 0) {
    return;
  }
  if (start_session()) {
    deadbeef->log("lxlyrics: restored the desktop-lyrics session (\"%s\" was set)",
                  LX_CONF_ENABLED);
    return;
  }
  /* The wanted state cannot be honoured (no app path, spawn failure): clear the
   * key so the menu item honestly reads off instead of claiming a session that
   * does not exist. */
  deadbeef->log("lxlyrics: cannot restore the session; clearing \"%s\"", LX_CONF_ENABLED);
  deadbeef->conf_set_int(LX_CONF_ENABLED, 0);
  deadbeef->sendmessage(DB_EV_ACTIONSCHANGED, 0, 0, 0);
}

/* The Preferences -> Plugins page writes these two keys through the host's own
 * config API while its dialog is live (upstream plugins/gtkui/pluginconf.c:
 * `.set_param = deadbeef->conf_set_str`) and then broadcasts DB_EV_CONFIGCHANGED
 * — the only notice a config-dialog edit gets. The wanted state IS the toggle
 * state, so a write has to reach the child: the key may not claim a session that
 * does not exist, and a session may not outlive a key that says off.
 *
 * `lxlyrics.app_path` is deliberately NOT acted on: the host re-applies the
 * whole dialog on every keystroke (`updates_immediately`), so restarting a
 * session here would respawn the app per character. The path is read by the next
 * start_session() instead, so editing it never disturbs a running child. */
static void sync_session_with_conf(void)
{
  if (!restore_attempted) {
    /* Before DB_EV_PLUGINSLOADED the streamer is not up yet; restore_session()
     * owns the first start. */
    return;
  }

  if (deadbeef->conf_get_int(LX_CONF_ENABLED, 0) != 0) {
    if (session_running()) {
      return;
    }
    if (!start_session()) {
      /* Same rule as restore_session(): a wanted state that cannot be honoured
       * is cleared, not left claiming a child that does not exist. The live
       * dialog keeps showing the tick the user clicked (gtkui does not re-read a
       * widget after a config write), but the key, the menu item and the child
       * agree again — and an entry that reports the same change on every
       * keystroke cannot retry the spawn. */
      deadbeef->log("lxlyrics: cannot start the session for \"%s\"; clearing it", LX_CONF_ENABLED);
      deadbeef->conf_set_int(LX_CONF_ENABLED, 0);
      deadbeef->sendmessage(DB_EV_ACTIONSCHANGED, 0, 0, 0);
    }
    return;
  }

  if (session_running()) {
    end_session("the \"" LX_CONF_ENABLED "\" config key was turned off");
  }
}

/* The persisted wanted state is restored by restore_session() on
 * DB_EV_PLUGINSLOADED (below); this action only flips it. start_session() sets
 * the key when the child is up, end_session() clears it when the session ends,
 * so the key always describes the real child. */
static int lxlyrics_toggle_action(DB_plugin_action_t* action, ddb_action_context_t context)
{
  (void)action;
  (void)context;
  if (deadbeef->conf_get_int(LX_CONF_ENABLED, 0) != 0) {
    end_session("the menu item was toggled off");
    return 0;
  }
  start_session(); /* the key follows the child; a failure leaves it off */
  return 0;
}

/* The player thread's playback mapping. The session's feed is borrowed once per
 * dispatch (session_feed_acquire() takes a reference under session_lock), so the
 * GTK thread may end the session while this runs without freeing the feed. */
static void handle_playback_event(lx_feed_t* feed, uint32_t id, uintptr_t ctx, uint32_t p1)
{
  switch (id) {
  case DB_EV_SONGSTARTED: {
    ddb_event_track_t* event = (ddb_event_track_t*)ctx;
    push_info(feed, event ? event->track : NULL);
    push_state(feed);
    break;
  }
  case DB_EV_SONGCHANGED: {
    ddb_event_trackchange_t* event = (ddb_event_trackchange_t*)ctx;
    push_info(feed, event ? event->to : NULL);
    push_state(feed);
    break;
  }
  case DB_EV_TRACKINFOCHANGED: {
    /* Only the playing track matters here: this event fires for every edited
     * track. The ctx track is refcounted for the duration of the callback. */
    ddb_event_track_t* event = (ddb_event_track_t*)ctx;
    /* Must not run inside pl_lock(): the safe accessor takes the streamer lock. */
    DB_playItem_t* playing = deadbeef->streamer_get_playing_track_safe();
    if (playing) {
      if (event && event->track == playing) {
        push_info(feed, playing);
      }
      deadbeef->pl_item_unref(playing);
    }
    break;
  }
  case DB_EV_PAUSED:
    if (p1) {
      lx_feed_send_pause(feed);
    } else {
      lx_feed_send_play(feed, current_position_ms());
    }
    break;
  case DB_EV_STOP:
    lx_feed_send_stop(feed);
    break;
  case DB_EV_SEEKED: {
    ddb_event_playpos_t* event = (ddb_event_playpos_t*)ctx;
    if (event) {
      lx_feed_send_play(feed, (int64_t)(event->playpos * 1000.0f));
    }
    break;
  }
  default:
    break;
  }
}

static int lxlyrics_message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2)
{
  (void)p2;
  switch (id) {
  case LX_EV_FEED_PROTOCOL_ERROR:
    /* §7: end the session, never respawn. end_session() also clears the wanted
     * state, which is what turns the menu item off. */
    end_session("the app violated the protocol");
    return 0;
  case LX_EV_FEED_CLOSE_REQUESTED:
    end_session("the lyric window was closed");
    return 0;
  case LX_EV_FEED_CHILD_EXITED:
    end_session("the app exited");
    return 0;
  case DB_EV_PLUGINSLOADED:
    restore_session();
    return 0;
  case DB_EV_CONFIGCHANGED:
    /* The configuration dialog and the installer write `lxlyrics.enabled` and
     * `lxlyrics.app_path` behind the plugin's back; the session has to follow
     * the key (see sync_session_with_conf). */
    sync_session_with_conf();
    return 0;
  default:
    break;
  }

  lx_feed_t* feed = session_feed_acquire();
  if (!feed) {
    return 0;
  }
  handle_playback_event(feed, id, ctx, p1);
  lx_feed_release(feed);
  return 0;
}

/* A slash in `title` is what places the item in gtkui's main menu; the toggle
 * state has no checkable flag, so it lives in the config (see end_session). */
static DB_plugin_action_t lxlyrics_action = {
  .title = "View/LX Lyrics",
  .name = "lxlyrics_toggle",
  .flags = DB_ACTION_COMMON | DB_ACTION_ADD_MENU,
  .callback2 = lxlyrics_toggle_action,
};

static DB_plugin_action_t* lxlyrics_get_actions(DB_playItem_t* track)
{
  return track ? NULL : &lxlyrics_action;
}

/* DeaDBeeF builds a plugin's configuration UI from this layout script: the host
 * renders it into Preferences -> Plugins -> Configuration and wires it to the
 * config API — `get_param` is `deadbeef->conf_get_str(key, def, …)`, `set_param`
 * is `deadbeef->conf_set_str(key, value)` (upstream
 * plugins/gtkui/prefwin/prefwinplugins.c), and closing the window runs
 * `conf_save()`. The widgets live in gtkui, so the string IS the dialog: this
 * module links no GTK and includes no GTK header. A NULL here is what hides the
 * whole panel (gtkui hides its button box with it, and the "Only show plugins
 * with configuration" filter drops the plugin), which is why this plugin had no
 * configuration at all.
 *
 * `entry`, not `file`: the app path's empty value means "search $PATH", and the
 * host's `file` widget is a read-only entry next to a file chooser (upstream
 * plugins/gtkui/pluginconf.c: `gtk_editable_set_editable (prop, FALSE)`) that
 * cannot express an empty path or take a pasted one. Both defaults are the
 * documented ones — empty app path, session off — and are what the host's
 * "Reset to defaults" writes. */
static const char lxlyrics_config_dialog[] =
  "property \"lx-lyrics-app binary (empty = search $PATH)\" entry " LX_CONF_APP_PATH " \"\";"
  "property \"Show desktop lyrics (the View/LX Lyrics toggle)\" checkbox " LX_CONF_ENABLED " 0;";

static int lxlyrics_start(void)
{
  /* Nothing is started here: the GTK side does not exist yet at load time. The
   * persisted wanted state is honoured by restore_session(), on the first
   * DB_EV_PLUGINSLOADED. */
  return 0;
}

static int lxlyrics_stop(void)
{
  /* Player shutting down: end the session and KEEP the wanted state, which is
   * what the next process restores (restore_session()). Clearing it here would
   * silently turn the user's desktop lyrics off across every restart. */
  stop_session();
  return 0;
}

static DB_misc_t lxlyrics_plugin = {
  DDB_REQUIRE_API_VERSION(1, 16).plugin.type = DB_PLUGIN_MISC,
  .plugin.version_major = 1,
  .plugin.version_minor = 0,
  .plugin.id = "lxlyrics",
  .plugin.name = "LX Lyrics",
  .plugin.descr = "Spawns the lx-lyrics desktop lyrics display and feeds it player state",
  .plugin.copyright = "Copyright (c) 2026 LX Lyrics contributors. GPL-3.0-only.",
  .plugin.start = lxlyrics_start,
  .plugin.stop = lxlyrics_stop,
  .plugin.get_actions = lxlyrics_get_actions,
  .plugin.message = lxlyrics_message,
  .plugin.configdialog = lxlyrics_config_dialog,
};

/* The loader derives this symbol from the module basename: ddb_lxlyrics.so. */
DB_plugin_t* ddb_lxlyrics_load(DB_functions_t* api)
{
  deadbeef = api;
  return DB_PLUGIN(&lxlyrics_plugin);
}
