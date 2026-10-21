/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * VLC adapter for the standalone lx-lyrics display: an interface module loaded
 * through `--extraintf=lxlyrics` (Preferences -> Interface -> Extra interface
 * modules) that lives in the VLC process, spawns `lx-lyrics-app --player-feed`
 * as its direct child (feed.c) and pushes the protocol v2 player feed described
 * in docs/protocol.md. It reads no lyrics — the app owns acquisition, parsing
 * and rendering.
 *
 * Everything VLC-specific lives here: the module boilerplate, the global-shortcut
 * helper (hotkey.c) and its toggle, the playback sampler (VLC 3.0 exposes no
 * analyser API a plugin can reach and no playback event callback that is safe
 * off the input thread, so the feed's 500 ms tick polls the public input
 * variables) and the state mapping (mapper.c). feed.c, hotkey.c and mapper.c
 * stay VLC-header-free and hermetically testable.
 */

/*
 * Out-of-tree plugin translation macros. In-tree VLC modules get `_()` and `N_()`
 * from include/vlc_fixups.h, which the build injects into the generated config.h
 * (configure.ac:713 `AH_BOTTOM([#include <vlc_fixups.h>])`); that header is a
 * *noinst* one (src/Makefile.am:114), so it is not part of the installed plugin
 * headers and an out-of-tree module has to provide the two macros itself. They
 * must come before the VLC headers: vlc_config_cat.h and the vlc_plugin.h
 * add_*() macros expand N_() while the headers are being parsed.
 */
#ifndef _
#define _(str) (str)
#endif
#ifndef N_
#define N_(str) (str)
#endif

/*
 * VLC plugin metadata, read by the plugin bank and exported as
 * vlc_entry_copyright__3_0_0f (include/vlc_plugin.h:541-555). It must be defined
 * before <vlc_plugin.h> is preprocessed, because that header decides there
 * whether to emit the export at all. No VLC_MODULE_LICENSE: VLC's macro set has
 * no GPL-3.0-only entry (only LGPL-2.1+ and GPL-2.0+), and this plugin is
 * GPL-3.0-only — the SPDX header above is the statement.
 */
#define VLC_MODULE_COPYRIGHT "Copyright (c) 2026 LX Lyrics contributors. GPL-3.0-only."

#include <vlc_common.h>
#include <vlc_config_cat.h>
#include <vlc_configuration.h>
#include <vlc_input.h>
#include <vlc_input_item.h>
#include <vlc_interface.h>
#include <vlc_mtime.h>
/* vlc_objects.h (vlc_object_release) is deliberately *not* included here: it is
 * the one VLC 3.0 header without an include guard and is meant to be pulled in
 * by vlc_common.h (which does, at vlc_common.h:1037). Including it again is a
 * hard error ("redefinition of 'struct vlc_object_t'"). */
#include <vlc_playlist.h>
#include <vlc_plugin.h>
#include <vlc_threads.h>
#include <vlc_url.h>
#include <vlc_variables.h>

#include "feed.h"
#include "hotkey.h"
#include "mapper.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Config key holding the wanted desktop-lyrics state (and the module's on/off switch). */
#define LX_CONF_ENABLED "lxlyrics-enabled"
/** Optional config key overriding where lx-lyrics-app lives. */
#define LX_CONF_APP_PATH "lxlyrics-app-path"
/** Name reported in `hello.host`. */
#define LX_HOST_NAME "vlc"
/** The display app, resolved on $PATH when LX_CONF_APP_PATH is empty. */
#define LX_APP_NAME "lx-lyrics-app"
/** The global-shortcut helper, resolved next to the app then on $PATH. */
#define LX_HOTKEY_NAME "lx-lyrics-hotkey"
/** Preferred trigger handed to the helper; the desktop owns the real binding. */
#define LX_HOTKEY_DEFAULT "CTRL+ALT+l"
/** How often the session thread re-reads the wanted state when nothing wakes
 * it, so a preferences write reaches an already-loaded module. */
#define LX_SESSION_POLL_MS 500
/** Buffer for a resolved app or helper path. */
#define LX_PATH_MAX 4096

/*
 * The running session, in intf_thread_t::p_sys (VLC's interface modules own
 * p_sys — the same pattern as modules/misc/audioscrobbler.c:79-101 and :392-418).
 *
 * Threading (feed.h carries the full contract):
 *
 * - VLC's main thread runs Open()/Close(): intf_Create() is called from
 *   libvlc_InternalAddIntf() while libvlc starts up (src/interface/interface.c:71
 *   and :198) and the matching module_unneed() from intf_DestroyAll()
 *   (src/interface/interface.c:238) as it shuts down. Open() only starts the
 *   session thread; Close() signals it and joins it.
 * - the session thread owns the feed object: it is the thread that spawns the
 *   app and therefore the only one allowed to stop it (feed.h). It must not be
 *   VLC's main thread, because lx_feed_stop() waits up to the feed's stop grace
 *   for the app to leave.
 * - the feed's reader thread runs `on_sample_status()` (the 500 ms tick) and the
 *   session-ending callbacks. It touches VLC only through the public,
 *   thread-safe getters in lx_sample(), and it never holds an object pointer
 *   across a callback: pl_CurrentInput() hands out a *reference*
 *   (include/vlc_interface.h:96-102, src/playlist/engine.c:375-378) that is
 *   released in the same poll tick.
 *
 * `mapper` is owned by the feed's reader thread — the feed's first sampler call
 * primes it and every later one updates it (feed.h) — so no lock guards it. The
 * fields guarded by `lock` are the ones shared between VLC's main thread, the
 * helper's reader thread and the feed's reader thread.
 */
struct intf_sys_t {
  intf_thread_t* intf;
  vlc_mutex_t lock;
  vlc_cond_t wait;
  /* Close(): stop the current feed (keeping the wanted state) and exit. */
  bool quit_requested;
  /* The helper's reader thread: the session thread must flip the wanted state.
   * The helper thread never calls config_PutInt() itself. */
  bool toggle_requested;
  /* The feed's reader thread: the session ended on its own. */
  bool end_requested;
  bool thread_started;
  vlc_thread_t thread;
  lx_hotkey_t* hotkey; /* VLC main thread (Open/Close) only */
  lx_mapper_t mapper;  /* feed reader thread only */
  char app_path[LX_PATH_MAX];
};

static int64_t lx_now_ms(void)
{
  /* mdate() is VLC 3's monotonic clock (include/vlc_threads.h:803); the mapper
   * only needs a monotonic millisecond clock for the seek heuristic. */
  return MS_FROM_VLC_TICK(mdate());
}

/*
 * The VLC playback sampler: everything the mapper needs, read from public VLC
 * APIs only, from the feed's reader thread.
 */
static void lx_sample(struct intf_sys_t* session, lx_mapper_sample_t* sample)
{
  memset(sample, 0, sizeof(*sample));
  sample->rate = 1.0;
  sample->now_ms = lx_now_ms();

  playlist_t* playlist = pl_Get(session->intf);
  /* The playlist owns the "fullscreen" bool (src/playlist/engine.c:471); the Qt
   * interface mirrors the video output's state onto it
   * (modules/gui/qt/input_manager.cpp:1290-1291, actions_manager.cpp:146). */
  sample->fullscreen = var_GetBool(playlist, "fullscreen");

  /* +1 reference, released before this function returns (include/vlc_interface.h:96). */
  input_thread_t* input = pl_CurrentInput(session->intf);
  if (input == NULL) {
    return; /* nothing playing: the sample keeps has_input == false */
  }
  sample->has_input = true;
  /* "state" (input_state_e, include/vlc_input.h:283) and "time" are plain
   * variables on the input object; VLC's variable system is thread-safe and the
   * reference above keeps the input alive throughout. "time" is in microseconds
   * — MS_FROM_VLC_TICK() is the documented conversion (include/vlc_mtime.h:49-56,
   * src/input/input.c:1867 divides the same variable by CLOCK_FREQ to get
   * seconds). */
  const int state = var_GetInteger(input, "state");
  switch (state) {
  case PLAYING_S:
  case OPENING_S:
    /* An opening input is about to play: reporting it as playing makes a track
     * change push set_info + set_play at once, instead of a set_stop/set_play
     * flicker (see the mapping table in the README). */
    sample->state = LX_PLAYER_PLAYING;
    break;
  case PAUSE_S:
    sample->state = LX_PLAYER_PAUSED;
    break;
  default: /* INIT_S, END_S, ERROR_S */
    sample->state = LX_PLAYER_STOPPED;
    break;
  }
  sample->played_time_ms = MS_FROM_VLC_TICK(var_GetInteger(input, "time"));
  sample->rate = (double)var_GetFloat(input, "rate");

  input_item_t* item = input_GetItem(input); /* include/vlc_input.h:543 */
  if (item != NULL) {
    /* `path` is the local file the app reads lyrics from; a stream (or anything
     * vlc_uri2path() cannot map to a path) yields "" (protocol §6). The item
     * getters lock the item's own mutex and return heap strings
     * (include/vlc_input_item.h:262-266 and the INPUT_META wrappers at :273-282). */
    char* uri = input_item_GetURI(item);
    char* path = (uri != NULL) ? vlc_uri2path(uri) : NULL; /* decodes %XX (vlc_url.h:58) */
    if (path != NULL) {
      snprintf(sample->path, sizeof(sample->path), "%s", path);
      free(path);
    }
    free(uri);
    char* artist = input_item_GetArtist(item);
    char* title = input_item_GetTitleFbName(item);
    char* album = input_item_GetAlbum(item);
    if (artist != NULL) {
      snprintf(sample->singer, sizeof(sample->singer), "%s", artist);
    }
    if (title != NULL) {
      snprintf(sample->name, sizeof(sample->name), "%s", title);
    }
    if (album != NULL) {
      snprintf(sample->album, sizeof(sample->album), "%s", album);
    }
    free(artist);
    free(title);
    free(album);
  }
  vlc_object_release(input);
}

/* Emits one mapped action. `feed` is the callback's own argument (borrowed), so
 * this is safe on whichever thread the feed calls us from. */
static void lx_emit(lx_feed_t* feed, lx_mapper_action_t action, const lx_mapper_sample_t* sample)
{
  switch (action) {
  case LX_ACTION_INFO:
    lx_feed_send_info(feed, sample->path, sample->singer, sample->name, sample->album,
                      sample->state == LX_PLAYER_PLAYING, sample->played_time_ms);
    break;
  case LX_ACTION_PLAY:
    lx_feed_send_play(feed, sample->played_time_ms);
    break;
  case LX_ACTION_PAUSE:
    lx_feed_send_pause(feed);
    break;
  case LX_ACTION_STOP:
    lx_feed_send_stop(feed);
    break;
  case LX_ACTION_FULLSCREEN:
    lx_feed_send_fullscreen(feed, sample->fullscreen);
    break;
  }
}

/* The feed's tick: called once right after the `hello` line (the prime, which
 * pushes the full set_info + state + fullscreen snapshot of protocol §5) and
 * every LX_FEED_STATUS_INTERVAL_MS after that (the periodic set_status). Runs on
 * the feed's reader thread. */
static bool on_sample_status(lx_feed_t* feed, void* userdata, bool* is_play,
                             int64_t* played_time_ms)
{
  struct intf_sys_t* session = userdata;
  lx_mapper_sample_t sample;
  lx_sample(session, &sample);

  lx_mapper_action_t actions[LX_MAPPER_MAX_ACTIONS];
  const size_t count = lx_mapper_update(&session->mapper, &sample, actions, LX_MAPPER_MAX_ACTIONS);
  for (size_t i = 0; i < count; i++) {
    lx_emit(feed, actions[i], &sample);
  }

  *is_play = sample.state == LX_PLAYER_PLAYING;
  *played_time_ms = sample.played_time_ms;
  return *is_play; /* set_status is periodic *while playing* (protocol §5) */
}

/* Asks the session thread to end the session. Called from the feed's reader
 * thread, which must not stop the feed itself (feed.h): this only flags and
 * wakes the session thread, which owns the feed. */
static void lx_end_session(struct intf_sys_t* session, const char* reason)
{
  msg_Info(session->intf, "ending the session (%s)", reason);
  vlc_mutex_lock(&session->lock);
  session->end_requested = true;
  vlc_cond_signal(&session->wait);
  vlc_mutex_unlock(&session->lock);
}

/* The user closed the lyric window (protocol §4): end the session, never
 * respawn, and leave the wanted state honestly off. */
static void on_close_requested(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  lx_end_session(userdata, "the lyric window was closed");
}

/* §7: the app violated the protocol. The feed has already closed its stdin so it
 * quits on EOF (§2); the host ends its session and never respawns it. */
static void on_protocol_error(lx_feed_t* feed, void* userdata, const char* reason)
{
  (void)feed;
  struct intf_sys_t* session = userdata;
  msg_Err(session->intf, "the app violated the protocol (%s)", reason ? reason : "unknown");
  lx_end_session(session, "the app violated the protocol");
}

static void on_child_exited(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  lx_end_session(userdata, "the app exited");
}

/* A "toggle" line arrives on the helper's reader thread. It only flags the
 * session thread — no config write and no other VLC state change here; that
 * thread owns the child and applies the flip in its poll loop. */
static void on_hotkey_toggle(void* userdata)
{
  struct intf_sys_t* session = userdata;
  vlc_mutex_lock(&session->lock);
  session->toggle_requested = true;
  vlc_cond_signal(&session->wait);
  vlc_mutex_unlock(&session->lock);
}

/* One status line ("ready <trigger>" or "unavailable <reason>"). Logging from
 * this thread is safe (VLC's logger takes its own lock); the helper writes each
 * line once, so an unavailable shortcut is reported once. */
static void on_hotkey_status(const char* line, void* userdata)
{
  struct intf_sys_t* session = userdata;
  const char* text = line ? line : "";
  if (strncmp(text, "ready ", 6) == 0) {
    msg_Info(session->intf, "global shortcut bound to %s", text + 6);
  } else if (strncmp(text, "unavailable ", 12) == 0) {
    msg_Warn(session->intf, "global shortcut unavailable (%s)", text + 12);
  } else if (text[0] != '\0') {
    msg_Info(session->intf, "hotkey helper: %s", text);
  }
}

/* Looks @p name up on $PATH. Returns false and writes "" when nothing
 * executable was found. */
static bool lx_find_program(const char* name, char* path, size_t path_size)
{
  const char* search = getenv("PATH");
  if (search == NULL) {
    path[0] = '\0';
    return false;
  }
  const size_t name_size = strlen(name) + 1;
  for (const char* dir = search; *dir;) {
    const char* end = strchr(dir, ':');
    size_t dir_len = (end != NULL) ? (size_t)(end - dir) : strlen(dir);
    if (dir_len > 0 && dir_len + 1 + name_size <= path_size) {
      memcpy(path, dir, dir_len);
      path[dir_len] = '/';
      memcpy(path + dir_len + 1, name, name_size);
      if (access(path, X_OK) == 0) {
        return true;
      }
    }
    if (end == NULL) {
      break;
    }
    dir = end + 1;
  }
  path[0] = '\0';
  return false;
}

/* $LX_CONF_APP_PATH when set, otherwise lx-lyrics-app on $PATH. */
static bool lx_resolve_app_path(intf_thread_t* intf, char* path, size_t path_size)
{
  char* configured = var_InheritString(intf, LX_CONF_APP_PATH);
  if (configured != NULL && *configured != '\0') {
    snprintf(path, path_size, "%s", configured);
    free(configured);
    return true;
  }
  free(configured);
  return lx_find_program(LX_APP_NAME, path, path_size);
}

/* The helper sits next to the installed app (tools/install.sh puts both in
 * <prefix>/bin); a non-standard app placement falls back to $PATH. */
static bool lx_resolve_hotkey_helper(const struct intf_sys_t* session, char* path, size_t path_size)
{
  const char* slash = strrchr(session->app_path, '/');
  const size_t dir_len = slash ? (size_t)(slash - session->app_path) : 0;
  if (dir_len > 0 && dir_len + 1 + sizeof(LX_HOTKEY_NAME) <= path_size) {
    memcpy(path, session->app_path, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1, LX_HOTKEY_NAME, sizeof(LX_HOTKEY_NAME));
    if (access(path, X_OK) == 0) {
      return true;
    }
  }
  return lx_find_program(LX_HOTKEY_NAME, path, path_size);
}

/* Starts the helper whenever the module is loaded — including while the window
 * is hidden, which is the point of the key. A missing helper or a failed spawn
 * is one warning; the adapter keeps working without a shortcut. Called from
 * Open(), i.e. VLC's own (uncloned) thread, so the helper does not inherit the
 * signal mask vlc_clone() gives the session thread. */
static void lx_start_hotkey(intf_thread_t* intf, struct intf_sys_t* session)
{
  char helper[LX_PATH_MAX];
  if (!lx_resolve_hotkey_helper(session, helper, sizeof(helper))) {
    msg_Warn(intf, "%s not found; the global shortcut is unavailable", LX_HOTKEY_NAME);
    return;
  }

  const lx_hotkey_config_t config = {
    .helper_path = helper,
    .accelerator = LX_HOTKEY_DEFAULT,
    .on_toggle = on_hotkey_toggle,
    .on_status = on_hotkey_status,
    .userdata = session,
  };
  session->hotkey = lx_hotkey_start(&config);
  if (session->hotkey == NULL) {
    msg_Warn(intf, "cannot spawn %s; the global shortcut is unavailable", helper);
  }
}

/* Applies the wanted state to the child: starts the app when the key says on
 * and nothing runs, stops it when the key says off and something does. Called
 * on the session thread only. */
static void lx_reconcile(struct intf_sys_t* session, lx_feed_t** feed,
                         const lx_feed_callbacks_t* callbacks)
{
  const bool wanted = config_GetInt(session->intf, LX_CONF_ENABLED) != 0;
  if (wanted && *feed == NULL) {
    /* The new feed's prime tick pushes the initial snapshot (feed.h); the
     * mapper must forget the previous session or that prime would diff as
     * "nothing changed". */
    lx_mapper_init(&session->mapper);
    *feed = lx_feed_spawn(session->app_path, LX_HOST_NAME, callbacks, session);
    if (*feed == NULL) {
      msg_Err(session->intf, "cannot spawn %s; clearing \"%s\"", session->app_path,
              LX_CONF_ENABLED);
      config_PutInt(session->intf, LX_CONF_ENABLED, 0);
    } else {
      msg_Info(session->intf, "starting the desktop-lyrics session (%s)", session->app_path);
    }
  } else if (!wanted && *feed != NULL) {
    lx_feed_stop(*feed);
    *feed = NULL;
    msg_Info(session->intf, "desktop lyrics are off (\"%s\" is false)", LX_CONF_ENABLED);
  }
}

/*
 * The session thread: the only thread that spawns and stops the feed (feed.h),
 * which keeps VLC's main thread out of the feed's stop grace. It runs one
 * session at a time and, between sessions, keeps the child reconciled with the
 * wanted state — the `lxlyrics-enabled` key — so the key and the window always
 * agree:
 *
 * - a "toggle" line only sets toggle_requested (on_hotkey_toggle); this thread
 *   flips the key and applies it, exactly like the preferences checkbox;
 * - close_requested, a protocol error or the app exiting ends the child and
 *   clears the key, but the module (and this thread) stay loaded and idle;
 * - a preferences write of the key is noticed by the 500 ms poll, because VLC
 *   does not signal a loaded module when its own option changes.
 *
 * Open() starts the thread whether the key is on or off: with it off the
 * module sits idle with the helper still running, which is what lets the key
 * open the window. Close() flags quit_requested; the wanted state is kept then,
 * so the next VLC start restores the window.
 */
static void* lx_session_run(void* data)
{
  struct intf_sys_t* session = data;
  const lx_feed_callbacks_t callbacks = {
    .close_requested = on_close_requested,
    .protocol_error = on_protocol_error,
    .child_exited = on_child_exited,
    .sample_status = on_sample_status,
  };
  lx_feed_t* feed = NULL;

  /* The usual case is a module loaded with the window wanted: start it now
   * rather than after the poll's first timeout. */
  lx_reconcile(session, &feed, &callbacks);

  for (;;) {
    vlc_mutex_lock(&session->lock);
    if (!session->quit_requested && !session->toggle_requested && !session->end_requested) {
      /* Timed, not indefinite: the timeout is the poll that sees a
       * preferences write of the wanted state. Everything else signals. */
      vlc_cond_timedwait(&session->wait, &session->lock,
                         mdate() + VLC_TICK_FROM_MS(LX_SESSION_POLL_MS));
    }
    const bool quit = session->quit_requested;
    const bool ended = session->end_requested;
    session->end_requested = false;
    /* A toggle that races the session's own end stays queued: the next pass
     * applies it to the now-hidden window instead of dropping the press. */
    bool toggle = false;
    if (!(ended && feed != NULL)) {
      toggle = session->toggle_requested;
      session->toggle_requested = false;
    }
    vlc_mutex_unlock(&session->lock);

    if (quit) {
      break;
    }

    if (ended && feed != NULL) {
      /* The app ended the session on its own (user close, protocol error, the
       * app quitting): stop it and turn the wanted state off. The module stays
       * loaded; the next toggle starts a fresh session. */
      lx_feed_stop(feed);
      feed = NULL;
      config_PutInt(session->intf, LX_CONF_ENABLED, 0);
      msg_Info(session->intf,
               "desktop lyrics are off; press the shortcut to show the window again");
      continue;
    }

    if (toggle) {
      /* The same switch the preferences checkbox drives: flip the wanted state,
       * then let the reconcile below start or stop the child. */
      const bool on = config_GetInt(session->intf, LX_CONF_ENABLED) != 0;
      config_PutInt(session->intf, LX_CONF_ENABLED, on ? 0 : 1);
    }

    lx_reconcile(session, &feed, &callbacks);
  }

  if (feed != NULL) {
    /* Close(): stop the child but keep the wanted state, which is what the next
     * VLC start restores. config_PutInt() marks the configuration dirty and VLC
     * writes it to vlcrc on exit (src/config/core.c:241, src/config/file.c:525,
     * src/libvlc.c:422). */
    lx_feed_stop(feed);
  }
  return NULL;
}

static int Open(vlc_object_t* obj)
{
  intf_thread_t* intf = (intf_thread_t*)obj;

  struct intf_sys_t* session = calloc(1, sizeof(*session));
  if (session == NULL) {
    return VLC_ENOMEM;
  }
  session->intf = intf;
  vlc_mutex_init(&session->lock);
  vlc_cond_init(&session->wait);
  lx_mapper_init(&session->mapper);
  intf->p_sys = session;

  if (!lx_resolve_app_path(intf, session->app_path, sizeof(session->app_path))) {
    msg_Err(intf, "%s not found; install it or set the \"%s\" config key", LX_APP_NAME,
            LX_CONF_APP_PATH);
    /* No session can ever start: say so in the wanted state instead of claiming
     * one (the same honesty rule the DeaDBeeF adapter's restore path follows). */
    config_PutInt(intf, LX_CONF_ENABLED, 0);
    return VLC_SUCCESS;
  }

  /* Both the helper and the session thread start whether the window is wanted
   * or not: the module being loaded is the adapter being on, and the window
   * follows the key (and the shortcut) from here on. The helper is spawned on
   * this thread — VLC's own, not one vlc_clone() created — so it does not
   * inherit the blocked signal mask described in the README. */
  lx_start_hotkey(intf, session);

  if (vlc_clone(&session->thread, lx_session_run, session, VLC_THREAD_PRIORITY_LOW) != 0) {
    msg_Err(intf, "cannot start the session thread");
    lx_hotkey_stop(session->hotkey);
    session->hotkey = NULL;
    config_PutInt(intf, LX_CONF_ENABLED, 0);
    return VLC_SUCCESS;
  }
  session->thread_started = true;
  msg_Info(intf, "the desktop-lyrics module is loaded (\"%s\" decides whether the window shows)",
           LX_CONF_ENABLED);
  return VLC_SUCCESS;
}

static void Close(vlc_object_t* obj)
{
  intf_thread_t* intf = (intf_thread_t*)obj;
  struct intf_sys_t* session = intf->p_sys;
  if (session == NULL) {
    return;
  }
  intf->p_sys = NULL;

  vlc_mutex_lock(&session->lock);
  /* VLC is shutting down: stop the child but keep the wanted state, so the next
   * VLC start restores the session instead of turning the user's desktop lyrics
   * off. A toggle is refused from here on. */
  session->quit_requested = true;
  vlc_cond_signal(&session->wait);
  vlc_mutex_unlock(&session->lock);

  /* Stop the helper before the session it toggles: lx_hotkey_stop() joins its
   * reader thread, and from then on nothing can flag a new toggle. */
  lx_hotkey_stop(session->hotkey);
  session->hotkey = NULL;

  if (session->thread_started) {
    /* Waits for the app to quit on the EOF its stdin just got (protocol §2) —
     * normally immediate, and bounded by the feed's stop grace. */
    vlc_join(session->thread, NULL);
  }
  vlc_cond_destroy(&session->wait);
  vlc_mutex_destroy(&session->lock);
  free(session);
}

/* VLC's module descriptor is a macro DSL: `vlc_module_begin()` opens the entry
 * function and the set_ and add_ macros append to it without separators, so
 * clang-format cannot parse the statement sequence and reflows it into an
 * unreadable stair. The descriptor therefore keeps VLC's own four-space layout
 * (like every module under modules/) inside a format-off block. */
// clang-format off
#define ENABLED_TEXT N_("Desktop lyrics")
#define ENABLED_LONGTEXT N_( \
    "Show the lx-lyrics desktop lyrics window for the playing track. " \
    "Honoured only while the module is loaded: see \"Extra interface modules\" " \
    "in \"Main interfaces\". Closing the lyrics window turns this off.")
#define APP_PATH_TEXT N_("Path to lx-lyrics-app")
#define APP_PATH_LONGTEXT N_( \
    "Full path of the lx-lyrics display application. " \
    "Leave empty to search $PATH.")

/*
 * The descriptor is the module's whole surface in VLC's own GUI, because VLC's
 * Simple settings pages cannot show an out-of-tree option at all: their panels
 * bind literal option names (`config_FindConfig("qt-notification")` and friends
 * in modules/gui/qt/components/simple_preferences.cpp) instead of enumerating
 * the config list. Everything a user can reach lives in "Show settings: All":
 *
 * - Interface -> Main interfaces -> "Extra interface modules" lists every banked
 *   module of this subcategory as a checkbox and uses `set_description()` as its
 *   label, so the description is what the user clicks to load the module.
 * - the same page carries the module's own node, labelled `set_shortname()` (the
 *   settings tree renders a module node labelled with the shortname and searches
 *   `set_description()`/`set_help()`/each option's *text* — never its name, so
 *   the option names alone cannot be searched for).
 * - `set_help()` is the text shown for that node, so it is the only place inside
 *   VLC that can spell out the enable+restart path.
 */
vlc_module_begin()
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_MAIN)
    set_shortname(N_("LX Lyrics"))
    set_description(N_("Desktop lyrics (lx-lyrics)"))
    set_help(N_( \
        "Loaded as the extra interface module \"lxlyrics\": tick it under " \
        "\"Extra interface modules\" in \"Main interfaces\", Save, then restart " \
        "VLC (that list is read once, while libvlc starts)."))
    add_bool(LX_CONF_ENABLED, true, ENABLED_TEXT, ENABLED_LONGTEXT, false)
    add_string(LX_CONF_APP_PATH, "", APP_PATH_TEXT, APP_PATH_LONGTEXT, false)
    /* Score 0: this module is never auto-selected. It only runs when the user
     * names it in "extraintf", which appends ",none" to the chain so VLC loads
     * exactly this module or none (src/libvlc.c:325-336). */
    set_capability("interface", 0)
    set_callbacks(Open, Close)
vlc_module_end()
  // clang-format on
