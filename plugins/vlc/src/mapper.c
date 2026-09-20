/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 */

#include "mapper.h"

#include <string.h>

void lx_mapper_init(lx_mapper_t* mapper)
{
  memset(mapper, 0, sizeof(*mapper));
}

/* The position multiplier for the seek heuristic: VLC's "rate" is a float in
 * [1/32, 32] (include/vlc_input.h:296-315), and protocol §5 only defines
 * 0.25-4.0, so anything else — including 0, negatives, NaN and infinities —
 * falls back to 1.0 rather than poisoning the prediction. */
static double lx_mapper_rate(const lx_mapper_sample_t* sample)
{
  const double rate = sample->rate;
  return (rate >= 0.25 && rate <= 4.0) ? rate : 1.0;
}

/* Position the last sample predicts for @p sample, in ms. */
static int64_t lx_mapper_expected_time(const lx_mapper_sample_t* previous,
                                       const lx_mapper_sample_t* sample)
{
  int64_t elapsed = sample->now_ms - previous->now_ms;
  if (elapsed < 0) {
    elapsed = 0; /* a clock that went backwards is not a seek */
  }
  return previous->played_time_ms + (int64_t)(lx_mapper_rate(sample) * (double)elapsed);
}

size_t lx_mapper_update(lx_mapper_t* mapper, const lx_mapper_sample_t* sample,
                        lx_mapper_action_t* actions, size_t max_actions)
{
  const lx_mapper_sample_t* previous = &mapper->previous;
  const bool first = !mapper->has_previous;
  size_t count = 0;

  /* `same_track` is what decides "track change" vs "metadata edit" vs "seek":
   * two samples describe the same track only when both carry an input and their
   * paths match (streams have an empty path, hence the has_input test). */
  const bool same_track =
    !first && previous->has_input && sample->has_input && strcmp(previous->path, sample->path) == 0;
  const bool same_info = same_track && strcmp(previous->singer, sample->singer) == 0 &&
                         strcmp(previous->name, sample->name) == 0 &&
                         strcmp(previous->album, sample->album) == 0;
  const bool track_changed = !same_track;
  const bool info_changed = first || (sample->has_input && !same_info);
  const bool state_changed = first || previous->state != sample->state;
  /* A track change also re-sends the state, so the app restarts its timer at the
   * new track's position. An input *disappearing* is not a track change: it is
   * already covered by the state change to stopped, and pushing set_stop again
   * would only repeat it. */
  const bool push_state = state_changed || (track_changed && sample->has_input);

  if (info_changed && count < max_actions) {
    actions[count++] = LX_ACTION_INFO;
  }
  if (push_state && count < max_actions) {
    switch (sample->state) {
    case LX_PLAYER_PLAYING:
      actions[count++] = LX_ACTION_PLAY;
      break;
    case LX_PLAYER_PAUSED:
      actions[count++] = LX_ACTION_PAUSE;
      break;
    default:
      actions[count++] = LX_ACTION_STOP;
      break;
    }
  } else if (!first && sample->has_input && sample->state == LX_PLAYER_PLAYING) {
    /* Still the same track, still playing: only a position jump that the elapsed
     * time cannot explain is a seek. */
    const int64_t expected = lx_mapper_expected_time(previous, sample);
    const int64_t drift = sample->played_time_ms - expected;
    if ((drift < 0 ? -drift : drift) > LX_MAPPER_SEEK_TOLERANCE_MS && count < max_actions) {
      actions[count++] = LX_ACTION_PLAY;
    }
  }
  if ((first || previous->fullscreen != sample->fullscreen) && count < max_actions) {
    actions[count++] = LX_ACTION_FULLSCREEN;
  }

  mapper->previous = *sample;
  mapper->has_previous = true;
  return count;
}
