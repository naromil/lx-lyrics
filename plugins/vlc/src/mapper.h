/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * VLC-header-free playback mapping for the protocol v2 feed: turns successive
 * polled playback samples (state, position, metadata, fullscreen) into the
 * host->app action sequence the feed must emit (docs/protocol.md §5).
 *
 * The VLC adapter has no playback event callbacks: VLC 3.0's input variables
 * ("state", "time", "rate") are read from the feed thread's 500 ms tick, so
 * everything the app needs — track change, seek, pause, stop, fullscreen — is
 * *diffed* from polled values here. Keeping that logic VLC-free makes it
 * testable without VLC (tests/mapper_test.c).
 */

#ifndef LX_MAPPER_H
#define LX_MAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Local path buffer, matching a `set_info.path` value (a filesystem path). */
#define LX_MAPPER_PATH_MAX 4096
/** Display metadata buffer (title/artist/album). */
#define LX_MAPPER_TEXT_MAX 1024

/** Actions one update can emit: set_info, a state action, set_fullscreen. */
#define LX_MAPPER_MAX_ACTIONS 4

/**
 * How far the polled position may drift from the position predicted by the last
 * sample before the adapter calls it a seek and re-syncs the app with
 * `set_play`. The poll tick is 500 ms and VLC updates its "time" variable a few
 * times per second, so a real seek is the only thing that moves the position by
 * more than this; a smaller jump needs no message at all, because the periodic
 * `set_status` carries the absolute position anyway.
 */
#define LX_MAPPER_SEEK_TOLERANCE_MS 2000

/** Playback state, collapsed from VLC's input_state_e (include/vlc_input.h:283). */
typedef enum {
  LX_PLAYER_STOPPED = 0, /* no input, still opening, ended or errored */
  LX_PLAYER_PLAYING,
  LX_PLAYER_PAUSED,
} lx_player_state_t;

/** One polled playback sample. All strings are UTF-8 and NUL-terminated. */
typedef struct {
  /** false when VLC has no current input: the metadata fields are then empty. */
  bool has_input;
  lx_player_state_t state;
  /** Position in ms; 0 when there is no input. */
  int64_t played_time_ms;
  /** Host monotonic clock in ms — the seek heuristic needs the elapsed time. */
  int64_t now_ms;
  /** Playback rate; out-of-range or non-finite values are treated as 1.0. */
  double rate;
  /** The player's main window is fullscreen. */
  bool fullscreen;
  /** Local filesystem path of the playing file; "" for streams and no input. */
  char path[LX_MAPPER_PATH_MAX];
  char singer[LX_MAPPER_TEXT_MAX];
  char name[LX_MAPPER_TEXT_MAX];
  char album[LX_MAPPER_TEXT_MAX];
} lx_mapper_sample_t;

/** One host->app action; the payload comes from the sample that produced it. */
typedef enum {
  LX_ACTION_INFO = 0,   /* set_info: path/singer/name/album/isPlay/played_time */
  LX_ACTION_PLAY,       /* set_play(played_time_ms) */
  LX_ACTION_PAUSE,      /* set_pause */
  LX_ACTION_STOP,       /* set_stop */
  LX_ACTION_FULLSCREEN, /* set_fullscreen(fullscreen) */
} lx_mapper_action_t;

typedef struct {
  lx_mapper_sample_t previous;
  bool has_previous;
} lx_mapper_t;

/** Resets @p mapper to "nothing pushed yet": the next update is the prime. */
void lx_mapper_init(lx_mapper_t* mapper);

/**
 * Diffs @p sample against the previous one and writes the actions to emit, in
 * the order they must be written, into @p actions (at most @p max_actions).
 * Returns how many were written. Every call advances the mapper's remembered
 * sample, so call it exactly once per sample and emit what it returns.
 *
 * The rules (docs/protocol.md §5):
 *
 * - **First update after lx_mapper_init()** — the prime: `set_info` +
 *   `set_play`/`set_pause`/`set_stop` + `set_fullscreen`, i.e. the full snapshot
 *   the app needs to render a session that starts during playback.
 * - **Track change** (the path changed, or an input appeared) — `set_info` +
 *   the state action, so the app re-acquires lyrics and restarts its timer.
 * - **Metadata edit** (same path, different singer/name/album) — `set_info`
 *   only: the timer must keep running.
 * - **State change** — `set_play(played_time)` / `set_pause` / `set_stop`.
 * - **Seek** (still playing, same track, position off the predicted one by more
 *   than LX_MAPPER_SEEK_TOLERANCE_MS) — `set_play(played_time)`. The prediction
 *   advances by `rate` per elapsed millisecond, so a non-1.0 rate is not
 *   mistaken for a seek.
 * - **Fullscreen** — `set_fullscreen` whenever it changes.
 */
size_t lx_mapper_update(lx_mapper_t* mapper, const lx_mapper_sample_t* sample,
                        lx_mapper_action_t* actions, size_t max_actions);

#endif /* LX_MAPPER_H */
