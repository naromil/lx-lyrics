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
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#include "feed.h"
#include "hotkey.h"
#include "session.h"
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
/** The settings page's "remember the state" checkbox: on restores a visible
 * lyrics window at the next player start; off starts the plugin enabled with
 * the window hidden (see init()). */
#define LX_CONF_REMEMBER_STATE "remember_state"
/** The preferred global-shortcut trigger handed to the helper at player start.
 * The desktop owns the real binding (KDE: System Settings -> Shortcuts). */
#define LX_CONF_HOTKEY "hotkey"
/** Optional override for where lx-lyrics-hotkey lives; empty means "next to
 * the app, then $PATH". */
#define LX_CONF_HOTKEY_HELPER "hotkey_helper"
/** Name reported in `hello.host`. */
#define LX_HOST_NAME "audacious"
/** The display app, resolved on $PATH when the config key is empty. */
#define LX_APP_NAME "lx-lyrics-app"
/** The global-shortcut helper, resolved next to the app then on $PATH. */
#define LX_HOTKEY_NAME "lx-lyrics-hotkey"
/** The registered default for the preferred trigger. */
#define LX_HOTKEY_DEFAULT "CTRL+ALT+l"
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
 * The Plugins row's information button: `PluginInfo.about` is what the
 * frontends key it off — `aud_plugin_has_about()` is literally `about !=
 * nullptr`, and the Qt list gives that row a "dialog-information" icon
 * (prefs-pluginlist-model.cc:159-163) whose click runs audqt::plugin_about()
 * (prefs-plugin.cc:41-61); the GTK/skins view adds the same "_About" button
 * (plugin-view.cc:202-207, :243-247). Both show a plain message dialog titled
 * "About <name>" with this text as the body, so it is written as plain text for
 * two frontends that render markup differently.
 */
static const char lx_about[] = N_(
  "Displays the playing track's lyrics in the standalone lx-lyrics desktop window.\n\n"
  "This plugin is transport only: it reports the playing file, its metadata, the playback state "
  "and position - plus the player's analyser frames - to a lx-lyrics-app child process over a "
  "pipe, and reads the two requests the app makes (an analyser frame, or that the user closed the "
  "window). The app acquires the lyrics itself (the sidecar .lrc next to the file, then the file's "
  "embedded tag), parses them and renders the window; no lyric is read, parsed or rendered "
  "here.\n\n"
  "Settings: the app-path entry says which binary to spawn (empty searches $PATH), \"Show or "
  "hide the lyrics window\" starts or stops the display without disabling this plugin, \"Open "
  "lyrics settings\" raises the app's own configuration dialog over the same pipe, and the "
  "remember-state checkbox decides whether the lyrics window is shown again at the next player "
  "start. A hidden lyrics window is not the plugin being off: playback is reported only while "
  "the window is showing, and the settings-page button brings it back.");

/*
 * The plugin's preferences page — the app path, a "remember the state" checkbox
 * and the way into the app's own configuration dialog.
 *
 * The widgets are declarative because one binary serves both frontends
 * (preferences.h:257 `struct PreferencesWidget`); WidgetString() binds a
 * section and a key (preferences.h:250), and every frontend wires the entry's
 * own "changed" signal straight to WidgetConfig::set_string() -> aud_set_str()
 * (prefs-widget-qt.cc:214-218, prefs-widget.cc:191-195). No init()/apply()
 * callback is needed: the key is written as the user types, and it is the same
 * `[lx-lyrics] app_path` resolve_app_path() reads. An empty value is the
 * documented "search $PATH" state, so clearing the field stays meaningful. The
 * checkbox is the same kind of binding — WidgetBool() over a section and a key
 * is read/written with aud_get_bool()/aud_set_bool() (preferences.cc:25-48),
 * and the defaults registered below decide what an untouched config shows.
 *
 * The buttons are the non-config widgets. "Show or hide the lyrics window" is
 * this adapter's own session switch: the plugin no longer disables itself when
 * the app goes away (§4), so this page is the explicit way back to a visible
 * window (the global shortcut does the same from any application). "Open
 * lyrics settings" reaches the app's own configuration dialog (37 keys) over
 * the feed instead of duplicating it. A button is declarative too — WidgetButton
 * carries the callback (preferences.h:299) — but the callback itself must be a
 * plain function, so both are declared here (the array needs their addresses)
 * and defined below the plugin class, which owns the session.
 */
static void lx_open_settings_clicked();
static void lx_toggle_clicked();

/* Registered so an untouched config reads "remember" and the reference
 * CTRL+ALT+l — aud_get_str() falls back to these defaults (config.cc:402-412),
 * which is what the page shows and what init() reads. */
static const char* const lx_config_defaults[] = {
  LX_CONF_REMEMBER_STATE, "TRUE", LX_CONF_HOTKEY, LX_HOTKEY_DEFAULT, nullptr,
};

static const PreferencesWidget lx_widgets[] = {
  WidgetLabel(N_("Path to the lx-lyrics-app executable; empty means search $PATH.")),
  WidgetEntry(N_("Application:"), WidgetString(LX_CONF_SECTION, LX_CONF_APP_PATH)),
  /* The shortcut is bound by the desktop (KDE: System Settings -> Shortcuts),
   * which is also where it is re-bound; this key is only the preferred trigger
   * sent to the helper at the next player start. */
  WidgetLabel(N_("Global shortcut: the preferred trigger (e.g. CTRL+ALT+l). The desktop assigns "
                 "the key and it is re-bound in the desktop's own shortcut settings (KDE: System "
                 "Settings -> Shortcuts); this value is used at the next player start.")),
  WidgetEntry(N_("Shortcut:"), WidgetString(LX_CONF_SECTION, LX_CONF_HOTKEY)),
  /* Off: a player start begins with the plugin enabled and the lyrics window
   * hidden instead of restoring one (see init()); the box only ever gates that
   * restore, a session the user asks for is always shown. */
  WidgetCheck(N_("Remember the desktop lyrics state from the last session"),
              WidgetBool(LX_CONF_SECTION, LX_CONF_REMEMBER_STATE)),
  WidgetButton(N_("Open lyrics settings"), WidgetVButton{lx_open_settings_clicked, nullptr}),
  WidgetButton(N_("Show or hide the lyrics window"), WidgetVButton{lx_toggle_clicked, nullptr}),
};

static constexpr PluginPreferences lx_prefs = {
  {lx_widgets}, // widgets
  nullptr,      // init: nothing to prepare
  nullptr,      // apply: the entry writes the key as it is typed
  nullptr,      // cleanup
};

/*
 * The plugin.
 *
 * Enabled is not the same as showing. Audacious' Plugins page enables and
 * disables plugins; an enabled general plugin is started at every player start
 * (plugin-init.cc:156-176 start_plugins -> start_plugins_two, called from
 * aud_run() at runtime.cc:343) and stopped on disable and at shutdown
 * (plugin-init.cc:31-43, :305-306). init() therefore prepares the session
 * state machine and cleanup() frees it, while the app itself is spawned only
 * when the window should be visible: a user toggle on this page, or the
 * remember-state restore at player start. When the app ends on its own (§4
 * close_requested, §7 protocol error, or an exit) the state machine drops to
 * hidden and the plugin stays enabled and idle — the playback hooks are
 * dissociated, so nothing respawns the window until the user asks again.
 *
 * The page `info.prefs` points at below is the one place the app path is
 * editable in the host's UI: Settings -> Plugins -> the settings icon in the LX
 * Lyrics row (Qt, prefs-window-qt.cc:625-638) or the Settings button after
 * selecting the row (GTK, plugin-view.cc:236). It is also the host's only route
 * to the app's *own* settings: the page's "Open lyrics settings" button sends
 * `open_settings` (§5), since the app owns its configuration and this page
 * carries just the app path and the remember-state checkbox. Both frontends render the same
 * declarative widgets (preferences.h:257 `struct PreferencesWidget`, :400
 * `struct PluginPreferences`) — the Qt one through audqt::prefs_populate()
 * (prefs-plugin.cc:130), the GTK one through audgui_create_widgets_with_domain()
 * (plugin-prefs.cc:174) — and both are reachable only while the plugin is
 * enabled, because the host gates them on `aud_plugin_has_configure(p) &&
 * aud_plugin_get_enabled(p)` (plugin-view.cc:170-175, prefs-window-qt.cc:627-628).
 * Keeping the plugin enabled while the window is hidden therefore also keeps
 * this page — and the toggle on it — reachable.
 *
 * `info.about` (lx_about) is the row's separate information button: the Qt list
 * paints an information icon in that column and the GTK/skins view adds an
 * "_About" button, both gated on `aud_plugin_has_about(p)` and both opening a
 * message dialog with that text (see lx_about).
 */
class LxLyrics : public GeneralPlugin {
public:
  static constexpr PluginInfo info = {
    N_("LX Lyrics"), // name
    "lx-lyrics",     // gettext domain (no catalogue: the name is used verbatim)
    lx_about,        // about: the Plugins row's information button (see lx_about)
    &lx_prefs,       // preferences page: the app path, the checkbox (see lx_widgets)
    0,               // flags: no main-loop restriction, this module uses neither GLib nor Qt
  };

  LxLyrics();

  bool init() override;
  void cleanup() override;

  /**
   * The app ended somewhere else: the user closed the lyric window
   * (`close_requested`, §4), the app violated the protocol (§7), or the app
   * exited. Called from the feed's reader thread; hops to the main thread and
   * tells the session state machine the window is gone. The plugin stays
   * enabled and idle, so nothing is respawned.
   */
  void request_end(const char* reason);

  /* lx_session_ops_t targets (the ops struct is built in init()). start()
   * resolves the app path and spawns the feed; stop() detaches the host hooks
   * and tears that feed down. Main thread only. */
  static void* session_start(void* userdata);
  static void session_stop(void* handle, void* userdata);

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
  /* The global-shortcut helper's callbacks. Both arrive on its reader thread;
   * toggle hops to the main thread before touching the session. */
  static void on_hotkey_toggle(void* userdata);
  static void on_hotkey_status(const char* line, void* userdata);

  void refresh();
  void push_info();
  void push_state();
  /* Host hooks + snapshot clock + analyser: registered only while the window
   * is showing, so a playback event can never respawn a hidden session. */
  void attach_hooks();
  void detach_hooks();
  bool resolve_app_path(char* path, size_t path_size);
  /* Spawns the helper when the plugin is enabled, even when no window is
   * showing — the key must work while the window is hidden. */
  void start_hotkey();
  bool resolve_hotkey_helper(char* path, size_t path_size);

  /* Both prefs-page buttons (lx_widgets) have to be plain functions — a
   * declarative PreferencesWidget holds a `void (*)()` — so they are declared
   * at file scope and friends of the session they drive. */
  friend void lx_open_settings_clicked();
  friend void lx_toggle_clicked();

  Timer<LxLyrics> m_timer;
  QueuedFunc m_end_queued;
  /* A helper "toggle" pressing the key: hops off the reader thread to this
   * (main-loop) queue, exactly like request_end(). */
  QueuedFunc m_toggle_queued;
  LxSpectrum m_spectrum;
  lx_feed_t* m_feed = nullptr;
  lx_hotkey_t* m_hotkey = nullptr;
  lx_session_t* m_session = nullptr;
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

/*
 * "Open lyrics settings" (the prefs page's button). A preferences callback runs
 * on the program's main thread — the frontends connect the widget's own clicked
 * signal (prefs-widget-qt.cc / plugin-prefs.cc), the same thread that spawns,
 * pushes and stops the session — and lx_feed_send_open_settings() is a no-op
 * with no session, so a click while the window is hidden does nothing instead
 * of needing a session just to reach the app's dialog.
 */
static void lx_open_settings_clicked()
{
  lx_feed_send_open_settings(aud_plugin_instance.m_feed);
}

/*
 * "Show or hide the lyrics window" (the prefs page's second button): the
 * explicit session switch. The window used to come and go with the plugin's
 * own enable state; now the plugin stays enabled, so this is the way back
 * without disabling anything. The global shortcut drives the same switch from
 * any application (see on_hotkey_toggle()).
 */
static void lx_toggle_clicked()
{
  if (!aud_plugin_instance.m_session) {
    AUDWARN("lx-lyrics: the session state is not ready; not toggling the window\n");
    return;
  }
  const bool showing = lx_session_toggle(aud_plugin_instance.m_session);
  AUDINFO("lx-lyrics: the lyrics window is now %s\n", showing ? "visible" : "hidden");
}

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

/* ---------------------------------------------------------- hotkey callbacks */

/* One "toggle" line from the helper: the same switch the settings page's
 * button drives. The helper's reader thread only queues; the state machine and
 * every aud_* call stay on the main thread (the request_end() idiom). */
void LxLyrics::on_hotkey_toggle(void* userdata)
{
  (void)userdata;
  aud_plugin_instance.m_toggle_queued.queue([]() {
    LxLyrics* self = &aud_plugin_instance;
    if (!self->m_session) {
      AUDWARN("lx-lyrics: the session state is not ready; not toggling the window\n");
      return;
    }
    const bool showing = lx_session_toggle(self->m_session);
    AUDINFO("lx-lyrics: the lyrics window is now %s\n", showing ? "visible" : "hidden");
  });
}

/* One status line ("ready <trigger>" or "unavailable <reason>"). Logging from
 * the reader thread matches the feed callbacks, which already do it. */
void LxLyrics::on_hotkey_status(const char* line, void* userdata)
{
  (void)userdata;
  const char* text = line ? line : "";
  if (strncmp(text, "ready ", 6) == 0) {
    AUDINFO("lx-lyrics: global shortcut bound to %s\n", text + 6);
  } else if (strncmp(text, "unavailable ", 12) == 0) {
    AUDWARN("lx-lyrics: global shortcut unavailable (%s)\n", text + 12);
  } else if (text[0] != '\0') {
    AUDINFO("lx-lyrics: hotkey helper: %s\n", text);
  }
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
    LxLyrics* self = &aud_plugin_instance;
    char reason[LX_REASON_SIZE * 2];
    pthread_mutex_lock(&self->m_request_lock);
    snprintf(reason, sizeof(reason), "%s", self->m_end_reason);
    pthread_mutex_unlock(&self->m_request_lock);
    const char* text = reason[0] ? reason : "the session ended";
    AUDWARN("lx-lyrics: the lyrics window is no longer showing (%s)\n", text);
    lx_session_app_ended(self->m_session, text);
  });
}

/* The session state machine's start target: resolve the app path, register the
 * host hooks for a visible window, spawn the feed and push the first snapshot
 * (protocol.md §3). Returns the feed handle (the state machine's opaque
 * handle), or nullptr when no session could be started. Main thread only. */
void* LxLyrics::session_start(void* userdata)
{
  auto* self = static_cast<LxLyrics*>(userdata);

  /* A new session invalidates every end request from the previous one; a
   * queued app_ended must not hide the window we are about to show. */
  self->m_end_queued.stop();
  pthread_mutex_lock(&self->m_request_lock);
  self->m_end_reason[0] = '\0';
  pthread_mutex_unlock(&self->m_request_lock);

  if (!self->resolve_app_path(self->m_app_path, sizeof(self->m_app_path))) {
    AUDERR("lx-lyrics: %s not found; install it or set \"%s\" in the [%s] section of "
           "$XDG_CONFIG_HOME/audacious/config\n",
           LX_APP_NAME, LX_CONF_APP_PATH, LX_CONF_SECTION);
    return nullptr;
  }

  /* Hooks and the snapshot clock first: they are all delivered on this thread,
   * so none can fire before the spawn below returns. They come straight back
   * off again if the spawn fails, leaving the plugin idle. */
  self->attach_hooks();

  const lx_feed_callbacks_t callbacks = {
    .analyser_data_requested = on_analyser_data_requested,
    .close_requested = on_close_requested,
    .protocol_error = on_protocol_error,
    .child_exited = on_child_exited,
    .sample_status = on_sample_status,
  };
  self->m_feed = lx_feed_spawn(self->m_app_path, LX_HOST_NAME, true, &callbacks, nullptr);
  if (!self->m_feed) {
    AUDERR("lx-lyrics: cannot spawn %s\n", self->m_app_path);
    self->detach_hooks();
    return nullptr;
  }
  AUDINFO("lx-lyrics: session started (%s)\n", self->m_app_path);

  /* The initial snapshot right after the spawn, so showing the window during
   * playback displays the current track at once. */
  self->refresh();
  self->push_info();
  self->push_state();
  return self->m_feed;
}

/* The session state machine's stop target: dissociate the host hooks before
 * the feed goes away (no frame or playback event may arrive mid-teardown),
 * then close the app's stdin and join/reap it (protocol.md §2). */
void LxLyrics::session_stop(void* handle, void* userdata)
{
  auto* self = static_cast<LxLyrics*>(userdata);
  self->detach_hooks();

  /* The reader thread reports child_exited while lx_feed_stop() joins it; that
   * is this teardown, not a new session end, so request_end() must ignore it.
   * cleanup() may already have set the flag, so restore what was there. */
  pthread_mutex_lock(&self->m_request_lock);
  const bool ending_before = self->m_ending;
  self->m_ending = true;
  pthread_mutex_unlock(&self->m_request_lock);

  lx_feed_t* feed = static_cast<lx_feed_t*>(handle);
  lx_feed_stop(feed);
  if (self->m_feed == feed) {
    self->m_feed = nullptr;
  }

  pthread_mutex_lock(&self->m_request_lock);
  self->m_ending = ending_before;
  pthread_mutex_unlock(&self->m_request_lock);
}

void LxLyrics::attach_hooks()
{
  hook_associate("playback begin", on_playback_begin, nullptr);
  hook_associate("playback stop", on_playback_stop, nullptr);
  hook_associate("playback pause", on_playback_pause, nullptr);
  hook_associate("playback unpause", on_playback_unpause, nullptr);
  hook_associate("playback seek", on_playback_seek, nullptr);
  hook_associate("tuple change", on_tuple_change, nullptr);
  m_timer.start();
  m_spectrum.start();
}

void LxLyrics::detach_hooks()
{
  /* Unregister the analyser before the feed goes away, so no frame can arrive
   * while the feed is released. Both stop paths are no-ops when hidden. */
  m_spectrum.stop();
  m_timer.stop();
  hook_dissociate("playback begin", on_playback_begin);
  hook_dissociate("playback stop", on_playback_stop);
  hook_dissociate("playback pause", on_playback_pause);
  hook_dissociate("playback unpause", on_playback_unpause);
  hook_dissociate("playback seek", on_playback_seek);
  hook_dissociate("tuple change", on_tuple_change);
}

/* Looks @p name up on $PATH. Main thread only (getenv()); returns false and
 * writes "" when nothing executable was found. */
static bool find_on_path(const char* name, char* path, size_t path_size)
{
  const char* search = getenv("PATH");
  if (!search) {
    return false;
  }
  const size_t name_size = strlen(name) + 1;
  for (const char* dir = search; *dir;) {
    const char* end = strchr(dir, ':');
    const size_t dir_len = end ? (size_t)(end - dir) : strlen(dir);
    if (dir_len > 0 && dir_len + 1 + name_size <= path_size) {
      memcpy(path, dir, dir_len);
      path[dir_len] = '/';
      memcpy(path + dir_len + 1, name, name_size);
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
  return find_on_path(LX_APP_NAME, path, path_size);
}

/* The "[lx-lyrics] hotkey_helper" key when set, otherwise lx-lyrics-hotkey
 * next to the resolved app (tools/install.sh puts both in <prefix>/bin), then
 * on $PATH. Main thread only. */
bool LxLyrics::resolve_hotkey_helper(char* path, size_t path_size)
{
  const String configured = aud_get_str(LX_CONF_SECTION, LX_CONF_HOTKEY_HELPER);
  const char* text = configured;
  if (text && text[0] != '\0') {
    snprintf(path, path_size, "%s", text);
    return access(path, X_OK) == 0;
  }

  char app[PATH_MAX];
  if (resolve_app_path(app, sizeof(app))) {
    const char* slash = strrchr(app, '/');
    const size_t dir_len = slash ? (size_t)(slash - app) : 0;
    if (dir_len > 0 && dir_len + 1 + sizeof(LX_HOTKEY_NAME) <= path_size) {
      memcpy(path, app, dir_len);
      path[dir_len] = '/';
      memcpy(path + dir_len + 1, LX_HOTKEY_NAME, sizeof(LX_HOTKEY_NAME));
      if (access(path, X_OK) == 0) {
        return true;
      }
    }
  }
  return find_on_path(LX_HOTKEY_NAME, path, path_size);
}

/* Starts the helper whenever the plugin is enabled — including when the
 * lyrics window stays hidden, which is the point of the key. A missing helper
 * or a failed spawn is one warning; the adapter keeps working without a
 * shortcut. */
void LxLyrics::start_hotkey()
{
  char helper[PATH_MAX];
  if (!resolve_hotkey_helper(helper, sizeof(helper))) {
    AUDWARN("lx-lyrics: %s not found; the global shortcut is unavailable\n", LX_HOTKEY_NAME);
    return;
  }

  const String preferred = aud_get_str(LX_CONF_SECTION, LX_CONF_HOTKEY);
  const char* trigger = preferred;
  const lx_hotkey_config_t config = {
    .helper_path = helper,
    .accelerator = (trigger && trigger[0] != '\0') ? trigger : nullptr,
    .on_toggle = on_hotkey_toggle,
    .on_status = on_hotkey_status,
    .userdata = this,
  };
  m_hotkey = lx_hotkey_start(&config);
  if (!m_hotkey) {
    AUDWARN("lx-lyrics: cannot spawn %s; the global shortcut is unavailable\n", helper);
  }
}

/* ------------------------------------------------------------- plugin entry */

bool LxLyrics::init()
{
  aud_config_set_defaults(LX_CONF_SECTION, lx_config_defaults);
  pthread_mutex_lock(&m_request_lock);
  m_ending = false;
  m_end_reason[0] = '\0';
  pthread_mutex_unlock(&m_request_lock);

  const lx_session_ops_t ops = {.start = session_start, .stop = session_stop};
  m_session = lx_session_new(&ops, this);
  if (!m_session) {
    AUDERR("lx-lyrics: cannot allocate the session state; staying enabled and idle\n");
    return true;
  }

  /* Before the remember_state restore below: enabled is enough for the key,
   * and its whole point is to open a window that starts hidden. */
  start_hotkey();

  /* The settings page's remember-state checkbox gates the player-start restore
   * only — the same shape as Fooyin's LxLyrics/RememberState. What tells that
   * restore apart from a session the user asks for is the interface:
   * start_plugins_two() starts the general plugins *before* the interface plugin
   * (plugin-init.cc:205-210), so no interface is current yet when the host
   * restores LX Lyrics, while the Plugins page — and audtool, and D-Bus — can
   * only ever enable it with one already running. A session the user asks for is
   * therefore always honoured, this run or after the box was flipped.
   * With remembering off, the restore starts the plugin enabled with the lyrics
   * window hidden (no spawn) instead of turning the plugin off: the user brings
   * it back with the settings page's toggle. A headless run starts no interface
   * at all, so there is nothing to tell the two apart by there and the checkbox
   * stays inert. */
  const bool restoring =
    !aud_get_headless_mode() && aud_plugin_get_current(PluginType::Iface) == nullptr;
  if (restoring && !aud_get_bool(LX_CONF_SECTION, LX_CONF_REMEMBER_STATE)) {
    AUDINFO("lx-lyrics: not restoring the lyrics window (remember_state is off); "
            "the plugin stays enabled and idle\n");
    return true;
  }

  if (!lx_session_show(m_session)) {
    AUDERR("lx-lyrics: the lyrics window could not be started; the plugin stays enabled and "
           "idle\n");
  }
  return true;
}

void LxLyrics::cleanup()
{
  /* Stop accepting session-end requests first: freeing the session below tears
   * the feed down, which makes the reader thread report child_exited; that must
   * not queue another teardown. */
  pthread_mutex_lock(&m_request_lock);
  m_ending = true;
  pthread_mutex_unlock(&m_request_lock);
  m_end_queued.stop();

  /* Stop the helper before the session it toggles: lx_hotkey_stop() joins the
   * reader thread, and a toggle it had already queued must not run against a
   * freed session. */
  lx_hotkey_stop(m_hotkey);
  m_hotkey = nullptr;
  m_toggle_queued.stop();

  /* lx_session_free() runs the stop target when the window is still showing:
   * detach the hooks, close stdin, join the reader and reap the app
   * (protocol.md §2). Safe when the session is already hidden. */
  lx_session_free(m_session);
  m_session = nullptr;

  pthread_mutex_lock(&m_request_lock);
  m_ending = false;
  m_end_reason[0] = '\0';
  pthread_mutex_unlock(&m_request_lock);
}
