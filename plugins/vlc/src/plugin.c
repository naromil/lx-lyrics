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
 * Everything VLC-specific lives here: the module boilerplate, the playback
 * sampler (VLC 3.0 exposes no analyser API a plugin can reach and no playback
 * event callback that is safe off the input thread, so the feed's 500 ms tick
 * polls the public input variables) and the state mapping (mapper.c). feed.c
 * and mapper.c stay VLC-header-free and hermetically testable.
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
/** Buffer for a resolved app path. */
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
 * fields guarded by `lock` are the ones shared between VLC's main thread and the
 * reader thread.
 */
struct intf_sys_t {
  intf_thread_t* intf;
  vlc_mutex_t lock;
  vlc_cond_t wait;
  lx_feed_t* feed;    /* published and cleared by the session thread, under lock */
  bool end_requested; /* guarded by lock: the session thread must end the session */
  bool keep_enabled;  /* guarded by lock: VLC is shutting down, keep the wanted state */
  bool thread_started;
  vlc_thread_t thread;
  lx_mapper_t mapper; /* feed reader thread only */
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

  const char* search = getenv("PATH");
  if (search == NULL) {
    path[0] = '\0';
    return false;
  }
  for (const char* dir = search; *dir;) {
    const char* end = strchr(dir, ':');
    size_t dir_len = (end != NULL) ? (size_t)(end - dir) : strlen(dir);
    if (dir_len > 0 && dir_len + 1 + sizeof(LX_APP_NAME) <= path_size) {
      memcpy(path, dir, dir_len);
      path[dir_len] = '/';
      memcpy(path + dir_len + 1, LX_APP_NAME, sizeof(LX_APP_NAME));
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

/*
 * The session thread: it spawns the app, waits for the session to end (the
 * feed's callbacks, or Close()), and then stops the feed. Keeping the spawn and
 * the stop on one thread satisfies feed.h's contract and keeps VLC's main thread
 * out of the feed's stop grace.
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

  /* The initial snapshot is pushed by the feed's prime tick, before
   * lx_feed_spawn() returns (feed.h). */
  lx_feed_t* feed = lx_feed_spawn(session->app_path, LX_HOST_NAME, &callbacks, session);

  vlc_mutex_lock(&session->lock);
  session->feed = feed;
  if (feed == NULL) {
    /* No session exists to wait for: this thread's job is done, and the wanted
     * state below has to be turned off. */
    session->end_requested = true;
  }
  while (!session->end_requested) {
    vlc_cond_wait(&session->wait, &session->lock);
  }
  const bool keep_enabled = session->keep_enabled;
  session->feed = NULL;
  vlc_mutex_unlock(&session->lock);

  if (feed != NULL) {
    /* Not under `lock`: lx_feed_stop() joins the feed's reader thread, whose
     * callbacks take that same lock (lx_end_session). */
    lx_feed_stop(feed);
  } else {
    msg_Err(session->intf, "cannot spawn %s", session->app_path);
  }

  if (!keep_enabled) {
    /* The wanted state lives in the config key, so it must honestly read off: a
     * session that ended (user close, protocol error, the app quitting, a failed
     * spawn) turns the key off, exactly like the DeaDBeeF adapter clears
     * lxlyrics.enabled. Only a VLC shutdown keeps it, which is what the next VLC
     * start restores. config_PutInt() marks the configuration dirty and VLC
     * writes it to vlcrc on exit (src/config/core.c:241, src/config/file.c:525,
     * src/libvlc.c:422). */
    config_PutInt(session->intf, LX_CONF_ENABLED, 0);
    msg_Info(session->intf,
             "desktop lyrics are off; set \"%s\" back to true and restart VLC to show them again",
             LX_CONF_ENABLED);
  }
  return NULL;
}

static int Open(vlc_object_t* obj)
{
  intf_thread_t* intf = (intf_thread_t*)obj;

  if (!var_InheritBool(intf, LX_CONF_ENABLED)) {
    msg_Info(intf, "desktop lyrics are off (\"%s\" is false)", LX_CONF_ENABLED);
    return VLC_SUCCESS; /* loaded, but idle: the key is the wanted state */
  }

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

  if (vlc_clone(&session->thread, lx_session_run, session, VLC_THREAD_PRIORITY_LOW) != 0) {
    msg_Err(intf, "cannot start the session thread");
    config_PutInt(intf, LX_CONF_ENABLED, 0);
    return VLC_SUCCESS;
  }
  session->thread_started = true;
  msg_Info(intf, "starting the desktop-lyrics session (%s)", session->app_path);
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
  /* VLC is shutting down: keep the wanted state, so the next VLC start restores
   * the session instead of silently turning the user's desktop lyrics off. */
  session->keep_enabled = true;
  session->end_requested = true;
  vlc_cond_signal(&session->wait);
  vlc_mutex_unlock(&session->lock);

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
