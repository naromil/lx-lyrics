/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * The protocol v2 "player feed" transport (docs/protocol.md): spawns the
 * lx-lyrics display app as a direct child with two pipes, writes host->app JSON
 * lines to its stdin and reads the two app->host actions from its stdout on a
 * dedicated thread.
 *
 * Pure POSIX C (no GLib, no DeaDBeeF): DeaDBeeF has no GLib main loop, and a
 * plain reader thread keeps this file hermetically testable (tests/feed_test.c).
 */

#ifndef LX_FEED_H
#define LX_FEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version written into every host->app line. */
#define LX_FEED_PROTOCOL_VERSION 2

/** Bytes in a `spectrum` frame (docs/protocol.md §5: exactly 128). */
#define LX_FEED_SPECTRUM_BYTES 128

/**
 * Cadence of the periodic `set_status` while playing. The host has no timer of
 * its own: the reader thread's poll() timeout doubles as the clock.
 */
#define LX_FEED_STATUS_INTERVAL_MS 500

/** An app->host line longer than this is a protocol error (protocol.md §7). */
#define LX_FEED_MAX_LINE (1024 * 1024)

typedef struct lx_feed lx_feed_t;

/**
 * Threading contract (protocol.md is a wire contract, not a threading one; this
 * is the adapter's).
 *
 * A session is touched from three threads at once, so the feed object is
 * reference-counted:
 *
 * - DeaDBeeF dispatches plugin message() on its own player thread (upstream
 *   src/main.c: `mainloop_tid = thread_start (mainloop_thread, NULL)`, whose
 *   player_mainloop() broadcasts `plugs[n]->message (msg, ctx, p1, p2)`);
 * - the GTK menu action callback runs on the GTK main thread (upstream
 *   plugins/gtkui/actions.c: `gdk_threads_add_idle (menu_action_cb, action)`
 *   -> `action->callback2 (action, DDB_ACTION_CTX_MAIN)`), i.e. the thread that
 *   starts and ends sessions;
 * - the feed's own reader thread runs the callbacks below.
 *
 * lx_feed_spawn() returns one reference (the spawner's). A thread that wants to
 * keep using a feed while another thread may end the session takes its own
 * reference with lx_feed_acquire() and drops it with lx_feed_release(); the
 * object is freed by the last release, so no use-after-free is possible across
 * lx_feed_stop(). Acquiring is only safe for a pointer that is itself protected
 * by a reference (e.g. read under the plugin's session lock).
 */
typedef struct {
  /**
   * The app asked for an analyser frame (`get_analyser_data_array`, only sent
   * when `hello.spectrum` was true). Reply with lx_feed_send_spectrum().
   */
  void (*analyser_data_requested)(lx_feed_t* feed, void* userdata);
  /** The user closed the lyric window; the app is quitting. */
  void (*close_requested)(lx_feed_t* feed, void* userdata);
  /**
   * The app's stdout carried a protocol violation (§7): a well-formed JSON line
   * with a missing/unsupported `v`, an unknown action, or a line over
   * LX_FEED_MAX_LINE. The session is already over when this is called (the
   * app's stdin was closed, so it quits on EOF per §2) and it is never
   * respawned: the host ends its session and turns its toggle off.
   */
  void (*protocol_error)(lx_feed_t* feed, void* userdata, const char* reason);
  /** The app's stdout reached EOF (or the stop grace expired): the session is over. */
  void (*child_exited)(lx_feed_t* feed, void* userdata);
  /**
   * Periodic status source, called every LX_FEED_STATUS_INTERVAL_MS while the
   * session is running. Fill @p is_play / @p played_time_ms and return true to
   * send a `set_status` line, or return false to skip this tick.
   */
  bool (*sample_status)(lx_feed_t* feed, void* userdata, bool* is_play, int64_t* played_time_ms);
} lx_feed_callbacks_t;

/**
 * Spawns @p app_path as `<app_path> --player-feed` and writes the `hello` line
 * as the very first host->app line. The child's stderr is left alone (the app
 * logs there); its stdout must carry protocol lines only — stray non-JSON lines
 * are tolerated and skipped *loudly* (§7), while a well-formed JSON line that
 * violates v2 ends the session through the protocol_error callback (§7).
 *
 * Returns NULL when the pipes or the process cannot be created. The returned
 * feed carries one reference for the caller (see the threading contract above).
 */
lx_feed_t* lx_feed_spawn(const char* app_path, const char* host_name, bool spectrum_available,
                         const lx_feed_callbacks_t* callbacks, void* userdata);

/**
 * Takes one reference to @p feed and returns it, so the caller may keep using
 * it across another thread's lx_feed_stop() (NULL stays NULL). Every acquire
 * must be balanced by exactly one lx_feed_release().
 */
lx_feed_t* lx_feed_acquire(lx_feed_t* feed);

/** Drops one reference, freeing the feed when the last one goes away. NULL is a no-op. */
void lx_feed_release(lx_feed_t* feed);

/**
 * Ends the session: closes the app's stdin (protocol.md §2: EOF on stdin makes
 * the app quit immediately), joins the reader thread, reaps the child, and then
 * drops the reference lx_feed_spawn() returned — the feed itself stays alive
 * until every lx_feed_acquire() has been released. Must be called from the
 * thread that called lx_feed_spawn(). Idempotent; passing NULL is a no-op.
 */
void lx_feed_stop(lx_feed_t* feed);

/* Host->app writers. All of them are no-ops when @p feed is NULL, so callers
 * do not need a "is a session running" check of their own. They are safe to
 * call from any thread for as long as the caller holds a reference (the
 * spawner's, or one from lx_feed_acquire()); writes are serialized by the
 * feed's mutex, and a writer on an ended session is a silent no-op. */

/** `set_info`: the full track snapshot (`path` is a local filesystem path). */
void lx_feed_send_info(lx_feed_t* feed, const char* path, const char* singer, const char* name,
                       const char* album, bool is_play, int64_t played_time_ms);
/** `set_status`: playback state + position refresh. */
void lx_feed_send_status(lx_feed_t* feed, bool is_play, int64_t played_time_ms);
/** `set_play`: resume/seek to @p time_ms. */
void lx_feed_send_play(lx_feed_t* feed, int64_t time_ms);
/** `set_pause`. */
void lx_feed_send_pause(lx_feed_t* feed);
/** `set_stop`. */
void lx_feed_send_stop(lx_feed_t* feed);
/**
 * `spectrum`: base64 of exactly LX_FEED_SPECTRUM_BYTES bytes. Other sizes are
 * the caller's bug: they are refused and logged.
 */
void lx_feed_send_spectrum(lx_feed_t* feed, const uint8_t* bytes, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* LX_FEED_H */
