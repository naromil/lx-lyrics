/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * Audacious adapter for the standalone lx-lyrics display: the plugin lives in
 * the player process, spawns the app as its direct child (feed.c) and pushes the
 * protocol v2 player feed described in docs/protocol.md. It reads no lyrics -
 * the app owns acquisition, parsing and rendering.
 *
 * The glue is C++ because libaudcore's plugin API is a C++ class hierarchy
 * (libaudcore/plugin.h:119 `class LIBAUDCORE_PUBLIC Plugin`, :498
 * `class LIBAUDCORE_PUBLIC GeneralPlugin`). The transport (feed.c, json.c) stays
 * pure POSIX C and never includes an Audacious header.
 */

#include <libaudcore/audstrings.h>
#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/mainloop.h>
#include <libaudcore/plugin.h>
#include <libaudcore/plugins.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#include "feed.h"
#include "spectrum.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Config section/key overriding where lx-lyrics-app lives (audacious.conf). */
#define LX_CONF_SECTION "lx-lyrics"
#define LX_CONF_APP_PATH "app_path"
/** Name reported in `hello.host`. */
#define LX_HOST_NAME "audacious"
/** The display app, resolved on $PATH when the config key is empty. */
#define LX_APP_NAME "lx-lyrics-app"
/** Metadata values are copied out of the playback tuple into buffers this big. */
#define LX_META_SIZE 1024
/** The session-end reason the feed thread hands to the main thread. */
#define LX_REASON_SIZE 160

/*
 * The playback snapshot.
 *
 * drct.h:27 warns that the aud_drct_* group is "not thread safe" — and the one
 * getter this adapter needs for `path`, aud_drct_get_filename(), reaches the
 * playlist lock (drct.cc:67-71 -> Playlist::playing_playlist(),
 * entry_filename()) — so every host call happens on the main thread: init(), the
 * hook handlers and the Hz4 timer below. The feed's reader thread (which owns
 * the 500 ms `set_status` clock) only reads this copy.
 */
struct LxSnapshot {
  bool playing;
  bool paused;
  int64_t played_time_ms;
  int samplerate;
  char path[PATH_MAX];
  char singer[LX_META_SIZE];
  char name[LX_META_SIZE];
  char album[LX_META_SIZE];
};

static pthread_mutex_t snapshot_lock = PTHREAD_MUTEX_INITIALIZER;
static LxSnapshot snapshot;

static void copy_meta(const Tuple& tuple, Tuple::Field field, char* value, size_t value_size)
{
  const String text = tuple.get_str(field);
  snprintf(value, value_size, "%s", text ? (const char*)text : "");
}

/* Fills @p out from the host. Main thread only. */
static void snapshot_take(LxSnapshot* out)
{
  memset(out, 0, sizeof(*out));
  out->playing = aud_drct_get_playing();
  out->paused = aud_drct_get_paused();
  out->played_time_ms = aud_drct_get_time();
  int bitrate = 0;
  int channels = 0;
  aud_drct_get_info(bitrate, out->samplerate, channels);
  if (!out->playing) {
    return;
  }
  /* The playlist holds a URI; `path` must be a local filesystem path, and
   * uri_to_filename() returns nothing at all for a non-file scheme
   * (audstrings.cc:622-637), which is exactly the "" of protocol.md §6 for a
   * stream or a CD track. */
  StringBuf path = uri_to_filename(aud_drct_get_filename(), false);
  if (path.len() > 0 && (size_t)path.len() < sizeof(out->path)) {
    snprintf(out->path, sizeof(out->path), "%s", (const char*)path);
  }
  const Tuple tuple = aud_drct_get_tuple();
  copy_meta(tuple, Tuple::Artist, out->singer, sizeof(out->singer));
  copy_meta(tuple, Tuple::Title, out->name, sizeof(out->name));
  copy_meta(tuple, Tuple::Album, out->album, sizeof(out->album));
}

static void snapshot_read(LxSnapshot* out)
{
  pthread_mutex_lock(&snapshot_lock);
  *out = snapshot;
  pthread_mutex_unlock(&snapshot_lock);
}

/*
 * The plugin.
 *
 * There is no toggle of its own: Audacious' Plugins settings page enables and
 * disables plugins, and an enabled general plugin is started at every player
 * start (plugin-init.cc:156-176 start_plugins -> start_plugins_two, called from
 * aud_run() at runtime.cc:343) and stopped on disable and at shutdown
 * (plugin-init.cc:31-43, :305-306). init() therefore starts the session and
 * cleanup() ends it, and turning the plugin's own enable state off is how this
 * adapter reports "no session" (§4/§7) — the same decision the Rhythmbox
 * adapter documents for its host.
 */
class LxLyrics : public GeneralPlugin {
public:
  static constexpr PluginInfo info = {
    N_("LX Lyrics"), // name
    "lx-lyrics",     // gettext domain (no catalogue: the name is used verbatim)
    nullptr,         // about
    nullptr,         // preferences page: none, the Plugins page is the toggle
    0,               // flags: no main-loop restriction, this module uses neither GLib nor Qt
  };

  LxLyrics();

  bool init() override;
  void cleanup() override;

  /**
   * The session ended somewhere else: the user closed the lyric window
   * (`close_requested`, §4), the app violated the protocol (§7), or the app
   * exited. Called from the feed's reader thread; hops to the main thread and
   * turns the plugin's enable state off, so nothing is respawned.
   */
  void request_end(const char* reason);

private:
  /* Playback hooks. All of them are delivered on the main thread. */
  static void on_playback_begin(void* data, void* user);
  static void on_playback_stop(void* data, void* user);
  static void on_playback_pause(void* data, void* user);
  static void on_playback_unpause(void* data, void* user);
  static void on_playback_seek(void* data, void* user);
  static void on_tuple_change(void* data, void* user);
  /* Feed callbacks. analyser_data_requested runs on the reader thread; the rest
   * arrive there too and only queue. */
  static void on_analyser_data_requested(lx_feed_t* feed, void* userdata);
  static void on_close_requested(lx_feed_t* feed, void* userdata);
  static void on_protocol_error(lx_feed_t* feed, void* userdata, const char* reason);
  static void on_child_exited(lx_feed_t* feed, void* userdata);
  static bool on_sample_status(lx_feed_t* feed, void* userdata, bool* is_play,
                               int64_t* played_time_ms);

  void refresh();
  void push_info();
  void push_state();
  void end_session();
  bool resolve_app_path(char* path, size_t path_size);

  Timer<LxLyrics> m_timer;
  QueuedFunc m_end_queued;
  LxSpectrum m_spectrum;
  lx_feed_t* m_feed = nullptr;
  PluginHandle* m_self = nullptr;
  char m_app_path[PATH_MAX] = {0};
  /* Guards m_ending/m_end_reason, which the feed's reader thread writes through
   * request_end() while the main thread reads them. */
  pthread_mutex_t m_request_lock = PTHREAD_MUTEX_INITIALIZER;
  bool m_ending = false;
  char m_end_reason[LX_REASON_SIZE] = {0};
};

/* The loader looks this symbol up by name (plugin-load.cc:75) and reads
 * magic/version/type straight out of it (plugin-load.cc:78-91); the module
 * basename becomes the plugin's id. audacious-plugins exports it the same way,
 * with EXPORT = __attribute__((visibility("default"))) (their meson.build:151-155). */
__attribute__((visibility("default"))) LxLyrics aud_plugin_instance;

LxLyrics::LxLyrics()
  : GeneralPlugin(info, false)
  , m_timer(TimerRate::Hz4, this, &LxLyrics::refresh)
{
}

/* ------------------------------------------------------------------ pushing */

void LxLyrics::refresh()
{
  LxSnapshot taken;
  snapshot_take(&taken);
  pthread_mutex_lock(&snapshot_lock);
  snapshot = taken;
  pthread_mutex_unlock(&snapshot_lock);
  m_spectrum.set_samplerate(taken.samplerate);
}

void LxLyrics::push_info()
{
  if (!m_feed) {
    return;
  }
  LxSnapshot current;
  snapshot_read(&current);
  /* The adapter carries no lyrics: the lyric keys stay empty so the app
   * acquires them from `path` (sidecar first, then embedded tags). */
  lx_feed_send_info(m_feed, current.path, current.singer, current.name, current.album,
                    current.playing && !current.paused, current.played_time_ms);
}

void LxLyrics::push_state()
{
  if (!m_feed) {
    return;
  }
  LxSnapshot current;
  snapshot_read(&current);
  if (current.playing && !current.paused) {
    lx_feed_send_play(m_feed, current.played_time_ms);
  } else if (current.playing) {
    lx_feed_send_pause(m_feed);
  } else {
    lx_feed_send_stop(m_feed);
  }
}

/* -------------------------------------------------------------- hook mapping */

/* "playback begin": a track starts playing — the playlist fires it from
 * Playlist::process_pending_update() (playlist.cc:207), both when playback
 * starts and when the playing position changes (playlist.cc:504/1032). */
void LxLyrics::on_playback_begin(void* data, void* user)
{
  (void)data;
  (void)user;
  if (!aud_plugin_instance.m_feed) {
    return;
  }
  aud_plugin_instance.refresh();
  aud_plugin_instance.push_info();
  aud_plugin_instance.push_state();
}

/* "playback stop" (playlist.cc:209/1037): playback stopped, clear the lyric. */
void LxLyrics::on_playback_stop(void* data, void* user)
{
  (void)data;
  (void)user;
  if (!aud_plugin_instance.m_feed) {
    return;
  }
  aud_plugin_instance.refresh();
  lx_feed_send_stop(aud_plugin_instance.m_feed);
}

/* "playback pause"/"playback unpause" (playback.cc:484). */
void LxLyrics::on_playback_pause(void* data, void* user)
{
  (void)data;
  (void)user;
  if (!aud_plugin_instance.m_feed) {
    return;
  }
  aud_plugin_instance.refresh();
  lx_feed_send_pause(aud_plugin_instance.m_feed);
}

void LxLyrics::on_playback_unpause(void* data, void* user)
{
  (void)data;
  (void)user;
  if (!aud_plugin_instance.m_feed) {
    return;
  }
  aud_plugin_instance.refresh();
  LxSnapshot current;
  snapshot_read(&current);
  lx_feed_send_play(aud_plugin_instance.m_feed, current.played_time_ms);
}

/*
 * "playback seek": a seek took effect — request_seek() queues it (playback.cc:268)
 * and so does the A-B/repeat loop (playback.cc:326). There is no need for the
 * Rhythmbox-style "a jump of a second is a seek" heuristic, the host reports the
 * event. The app resumes its lyric timer on set_play (§5), so a seek while
 * paused is followed by set_pause: LyricPlayer::play() resyncs its clock and
 * selects the line at the seek target, pause() then stops the timer again.
 */
void LxLyrics::on_playback_seek(void* data, void* user)
{
  (void)data;
  (void)user;
  if (!aud_plugin_instance.m_feed) {
    return;
  }
  aud_plugin_instance.refresh();
  LxSnapshot current;
  snapshot_read(&current);
  lx_feed_send_play(aud_plugin_instance.m_feed, current.played_time_ms);
  if (current.paused) {
    lx_feed_send_pause(aud_plugin_instance.m_feed);
  }
}

/* "tuple change" (playback.cc:146): the playback tuple was updated — a tag edit
 * or the metadata a decoder reports late. Only the playing track is pushed. */
void LxLyrics::on_tuple_change(void* data, void* user)
{
  (void)data;
  (void)user;
  if (!aud_plugin_instance.m_feed) {
    return;
  }
  aud_plugin_instance.refresh();
  LxSnapshot current;
  snapshot_read(&current);
  if (current.playing) {
    aud_plugin_instance.push_info();
  }
}

/* ---------------------------------------------------------- feed callbacks */

void LxLyrics::on_analyser_data_requested(lx_feed_t* feed, void* userdata)
{
  (void)userdata;
  uint8_t frame[LX_FEED_SPECTRUM_BYTES];
  if (aud_plugin_instance.m_spectrum.read(frame, sizeof(frame))) {
    lx_feed_send_spectrum(feed, frame, sizeof(frame));
  }
}

void LxLyrics::on_close_requested(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  (void)userdata;
  aud_plugin_instance.request_end("the lyric window was closed");
}

void LxLyrics::on_protocol_error(lx_feed_t* feed, void* userdata, const char* reason)
{
  (void)feed;
  (void)userdata;
  AUDERR("lx-lyrics: the app violated the protocol (%s)\n", reason ? reason : "unknown");
  aud_plugin_instance.request_end("the app violated the protocol");
}

void LxLyrics::on_child_exited(lx_feed_t* feed, void* userdata)
{
  (void)feed;
  (void)userdata;
  aud_plugin_instance.request_end("the app exited");
}

/*
 * The periodic `set_status` source, called on the feed's reader thread every
 * LX_FEED_STATUS_INTERVAL_MS. It reads the snapshot the main thread refreshes at
 * Hz4 (250 ms) — the reader thread never calls an aud_* function itself.
 */
bool LxLyrics::on_sample_status(lx_feed_t* feed, void* userdata, bool* is_play,
                                int64_t* played_time_ms)
{
  (void)feed;
  (void)userdata;
  LxSnapshot current;
  snapshot_read(&current);
  if (!current.playing || current.paused) {
    return false;
  }
  *is_play = true;
  *played_time_ms = current.played_time_ms;
  return true;
}

/* -------------------------------------------------------------- session I/O */

void LxLyrics::request_end(const char* reason)
{
  pthread_mutex_lock(&m_request_lock);
  const bool ignored = m_ending || m_end_reason[0] != '\0';
  if (!ignored) {
    snprintf(m_end_reason, sizeof(m_end_reason), "%s", reason ? reason : "the session ended");
  }
  pthread_mutex_unlock(&m_request_lock);
  if (ignored) {
    /* A teardown is already running (or already queued): the first reason is
     * the one worth reporting. */
    return;
  }
  /* Hop to the main thread, where every aud_* call belongs. QueuedFunc is the
   * host's own cross-thread mechanism — event_queue() is built on it
   * (eventqueue.cc:71-78) — and its registry is a mutex-guarded hash table
   * (mainloop.cc:70-71). */
  m_end_queued.queue([]() {
    aud_plugin_instance.end_session();
  });
}

/* Turns the plugin's own enable state off, which is what makes the Plugins page
 * honestly read off (§4/§7: end the session, never respawn). aud_plugin_enable()
 * then runs cleanup() — the single teardown path — and is a no-op when the
 * plugin is already disabled (plugin-init.cc:313-317). */
void LxLyrics::end_session()
{
  char reason[LX_REASON_SIZE];
  pthread_mutex_lock(&m_request_lock);
  snprintf(reason, sizeof(reason), "%s", m_end_reason);
  pthread_mutex_unlock(&m_request_lock);
  AUDWARN("lx-lyrics: ending the session (%s)\n", reason[0] ? reason : "the session ended");
  if (!m_self) {
    return;
  }
  aud_plugin_enable(m_self, false);
}

/* The "[lx-lyrics] app_path" config key when set, otherwise lx-lyrics-app on
 * $PATH. Main thread only (aud_get_str() is part of the host's config API). */
bool LxLyrics::resolve_app_path(char* path, size_t path_size)
{
  const String configured = aud_get_str(LX_CONF_SECTION, LX_CONF_APP_PATH);
  const char* text = configured;
  if (text && text[0] != '\0') {
    snprintf(path, path_size, "%s", text);
    return true;
  }
  const char* search = getenv("PATH");
  if (!search) {
    return false;
  }
  for (const char* dir = search; *dir;) {
    const char* end = strchr(dir, ':');
    const size_t dir_len = end ? (size_t)(end - dir) : strlen(dir);
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

/* ------------------------------------------------------------- plugin entry */

bool LxLyrics::init()
{
  m_self = aud_plugin_by_header(&aud_plugin_instance);
  if (!m_self) {
    AUDERR("lx-lyrics: cannot resolve our own plugin handle; not starting a session\n");
    return true;
  }
  pthread_mutex_lock(&m_request_lock);
  m_ending = false;
  m_end_reason[0] = '\0';
  pthread_mutex_unlock(&m_request_lock);

  if (!resolve_app_path(m_app_path, sizeof(m_app_path))) {
    AUDERR("lx-lyrics: %s not found; install it or set \"%s\" in the [%s] section of "
           "$XDG_CONFIG_HOME/audacious/config\n",
           LX_APP_NAME, LX_CONF_APP_PATH, LX_CONF_SECTION);
    /* No session, so the toggle must not claim one: end_session() is queued
     * because we are inside start_plugin() right now. */
    request_end("the display app was not found");
    return true;
  }

  /* Hooks and the snapshot clock first: they are all delivered on this thread,
   * so they cannot fire before the spawn below returns. */
  hook_associate("playback begin", on_playback_begin, nullptr);
  hook_associate("playback stop", on_playback_stop, nullptr);
  hook_associate("playback pause", on_playback_pause, nullptr);
  hook_associate("playback unpause", on_playback_unpause, nullptr);
  hook_associate("playback seek", on_playback_seek, nullptr);
  hook_associate("tuple change", on_tuple_change, nullptr);
  m_timer.start();
  m_spectrum.start();

  const lx_feed_callbacks_t callbacks = {
    .analyser_data_requested = on_analyser_data_requested,
    .close_requested = on_close_requested,
    .protocol_error = on_protocol_error,
    .child_exited = on_child_exited,
    .sample_status = on_sample_status,
  };
  m_feed = lx_feed_spawn(m_app_path, LX_HOST_NAME, true, &callbacks, nullptr);
  if (!m_feed) {
    AUDERR("lx-lyrics: cannot spawn %s\n", m_app_path);
    request_end("the display app could not be spawned");
    return true;
  }
  AUDINFO("lx-lyrics: session started (%s)\n", m_app_path);

  /* Push the initial snapshot right after the spawn (protocol.md §3), so
   * enabling the plugin during playback shows the current track at once. */
  refresh();
  push_info();
  push_state();
  return true;
}

void LxLyrics::cleanup()
{
  /* Stop accepting session-end requests first: lx_feed_stop() below makes the
   * reader thread report child_exited, which must not queue another teardown. */
  pthread_mutex_lock(&m_request_lock);
  m_ending = true;
  pthread_mutex_unlock(&m_request_lock);
  m_end_queued.stop();

  /* Unregister the analyser before the child goes away, so no frame can arrive
   * while the feed is released. */
  m_spectrum.stop();
  m_timer.stop();
  hook_dissociate("playback begin", on_playback_begin);
  hook_dissociate("playback stop", on_playback_stop);
  hook_dissociate("playback pause", on_playback_pause);
  hook_dissociate("playback unpause", on_playback_unpause);
  hook_dissociate("playback seek", on_playback_seek);
  hook_dissociate("tuple change", on_tuple_change);

  /* protocol.md §2: closing the app's stdin is how it is told the player is
   * gone. lx_feed_stop() joins the reader thread, gives the app
   * LX_FEED_STOP_GRACE_MS to leave on its own and reaps it. */
  lx_feed_stop(m_feed);
  m_feed = nullptr;

  pthread_mutex_lock(&m_request_lock);
  m_ending = false;
  m_end_reason[0] = '\0';
  pthread_mutex_unlock(&m_request_lock);
}
