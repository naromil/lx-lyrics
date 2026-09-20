/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * The protocol v2 "player feed" transport (docs/protocol.md): spawns the
 * lx-lyrics display app as a direct child with two pipes, writes host->app JSON
 * lines to its stdin and reads the app->host actions from its stdout on a
 * dedicated thread.
 *
 * Pure POSIX C (no VLC headers at all): the VLC glue lives in plugin.c, and a
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

/**
 * Cadence of the periodic `set_status` while playing. The host has no timer of
 * its own: the reader thread's poll() timeout doubles as the clock (and as the
 * playback sampler's tick — see lx_feed_callbacks_t.sample_status).
 */
#define LX_FEED_STATUS_INTERVAL_MS 500

/** An app->host line longer than this is a protocol error (protocol.md §7). */
#define LX_FEED_MAX_LINE (1024 * 1024)

typedef struct lx_feed lx_feed_t;

/**
 * Threading contract (protocol.md is a wire contract, not a threading one; this
 * is the adapter's).
 *
 * The VLC adapter runs the feed from three threads:
 *
 * - VLC's main thread, which creates and destroys every interface module:
 *   `intf_Create()` is called from `libvlc_InternalAddIntf()`
 *   (src/interface/interface.c:71 and :198) while libvlc is being initialised,
 *   and the matching `module_unneed()` (which runs the module's Close) from
 *   `intf_DestroyAll()` (src/interface/interface.c:238) during
 *   libvlc_InternalDestroy;
 * - the adapter's session thread (plugin.c), which calls lx_feed_spawn() and
 *   lx_feed_stop() and therefore satisfies the "stop from the spawning thread"
 *   rule below without blocking VLC's main thread;
 * - this feed's own reader thread, which runs the callbacks below.
 *
 * lx_feed_spawn() returns one reference (the spawner's). A thread that wants to
 * keep using a feed while another thread may end the session takes its own
 * reference with lx_feed_acquire() and drops it with lx_feed_release(); the
 * object is freed by the last release, so no use-after-free is possible across
 * lx_feed_stop(). Acquiring is only safe for a pointer that is itself protected
 * by a reference (e.g. read under the adapter's session lock).
 */
typedef struct {
  /** The user closed the lyric window; the app is quitting. */
  void (*close_requested)(lx_feed_t* feed, void* userdata);
  /**
   * The app's stdout carried a protocol violation (§7): a well-formed JSON line
   * with a missing/unsupported `v`, an unknown action, a malformed payload or a
   * line over LX_FEED_MAX_LINE. The session is already over when this is called
   * (the app's stdin was closed, so it quits on EOF per §2) and it is never
   * respawned: the host ends its session and turns its toggle off.
   */
  void (*protocol_error)(lx_feed_t* feed, void* userdata, const char* reason);
  /** The app's stdout reached EOF (or the stop grace expired): the session is over. */
  void (*child_exited)(lx_feed_t* feed, void* userdata);
  /**
   * The playback sampler. It is called **once right after the `hello` line** —
   * that call is the prime, and it is where the adapter pushes the initial
   * snapshot (protocol §3/§5: `set_info` + the state action + `set_fullscreen`,
   * so a session that starts during playback renders the current track). Its
   * return value is ignored on that first call, because `set_info` already
   * carries the state and position; afterwards it is called every
   * LX_FEED_STATUS_INTERVAL_MS while the session runs, and there its result
   * decides whether a `set_status` line follows (protocol §5: periodic while
   * playing). The adapter samples VLC here — this is its only playback clock,
   * because VLC 3.0 exposes no callback for "the position changed" that is safe
   * off the input thread.
   */
  bool (*sample_status)(lx_feed_t* feed, void* userdata, bool* is_play, int64_t* played_time_ms);
} lx_feed_callbacks_t;

/**
 * Spawns @p app_path as `<app_path> --player-feed` and writes the `hello` line
 * as the very first host->app line: `{"v":2,"action":"hello","host":<host_name>,
 * "spectrum":false}`. `spectrum` is always false because VLC 3.0 exposes no
 * analyser API a background interface module can reach (see the README); §4
 * therefore forbids the app from asking for analyser frames, and such a request
 * is treated as the §7 protocol error it is.
 *
 * The child's stderr is left alone (the app logs there); its stdout must carry
 * protocol lines only — stray non-JSON lines are tolerated and skipped *loudly*
 * (§7), while a well-formed JSON line that violates v2 ends the session through
 * the protocol_error callback (§7).
 *
 * Returns NULL when the pipes or the process cannot be created. The returned
 * feed carries one reference for the caller (see the threading contract above).
 */
lx_feed_t* lx_feed_spawn(const char* app_path, const char* host_name,
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
 * thread that called lx_feed_spawn(), and never from the reader thread itself
 * (which this call joins). Idempotent; passing NULL is a no-op.
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
/** `set_fullscreen`: the player's main window entered/left fullscreen. */
void lx_feed_send_fullscreen(lx_feed_t* feed, bool is_fullscreen);

#ifdef __cplusplus
}
#endif

#endif /* LX_FEED_H */
