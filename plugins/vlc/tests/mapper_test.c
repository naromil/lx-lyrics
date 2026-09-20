/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * Hermetic tests for the VLC-free playback mapping (src/mapper.c): the decision
 * logic that turns polled playback samples into protocol v2 actions. No VLC, no
 * child process, no display server.
 */

#define _POSIX_C_SOURCE 200809L

#include "mapper.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool ok, const char* what)
{
  if (ok) {
    printf("ok   %s\n", what);
    return;
  }
  failures++;
  printf("FAIL %s\n", what);
}

static const char* action_name(lx_mapper_action_t action)
{
  switch (action) {
  case LX_ACTION_INFO:
    return "set_info";
  case LX_ACTION_PLAY:
    return "set_play";
  case LX_ACTION_PAUSE:
    return "set_pause";
  case LX_ACTION_STOP:
    return "set_stop";
  case LX_ACTION_FULLSCREEN:
    return "set_fullscreen";
  }
  return "?";
}

/* One sample, with everything a playing track needs. */
static lx_mapper_sample_t playing_sample(const char* path, int64_t time_ms, int64_t now_ms)
{
  lx_mapper_sample_t sample;
  memset(&sample, 0, sizeof(sample));
  sample.has_input = true;
  sample.state = LX_PLAYER_PLAYING;
  sample.played_time_ms = time_ms;
  sample.now_ms = now_ms;
  sample.rate = 1.0;
  snprintf(sample.path, sizeof(sample.path), "%s", path);
  snprintf(sample.singer, sizeof(sample.singer), "Singer");
  snprintf(sample.name, sizeof(sample.name), "Song");
  snprintf(sample.album, sizeof(sample.album), "Album");
  return sample;
}

/* An empty sample: nothing playing at all. */
static lx_mapper_sample_t idle_sample(int64_t now_ms)
{
  lx_mapper_sample_t sample;
  memset(&sample, 0, sizeof(sample));
  sample.now_ms = now_ms;
  sample.rate = 1.0;
  return sample;
}

static size_t update(lx_mapper_t* mapper, const lx_mapper_sample_t* sample,
                     lx_mapper_action_t* actions)
{
  return lx_mapper_update(mapper, sample, actions, LX_MAPPER_MAX_ACTIONS);
}

/* Runs one update and compares the action list with the expectation. */
static void expect(const char* what, lx_mapper_t* mapper, const lx_mapper_sample_t* sample,
                   const lx_mapper_action_t* expected, size_t expected_count)
{
  lx_mapper_action_t actions[LX_MAPPER_MAX_ACTIONS];
  const size_t count = update(mapper, sample, actions);
  if (count != expected_count) {
    failures++;
    printf("FAIL %s: expected %zu action(s), got %zu", what, expected_count, count);
    for (size_t i = 0; i < count; i++) {
      printf(" %s", action_name(actions[i]));
    }
    printf("\n");
    return;
  }
  for (size_t i = 0; i < count; i++) {
    if (actions[i] != expected[i]) {
      failures++;
      printf("FAIL %s: action %zu is %s, expected %s\n", what, i, action_name(actions[i]),
             action_name(expected[i]));
      return;
    }
  }
  check(true, what);
}

/* Consumes the prime so the case under test starts from a known previous sample.
 * The prime itself is covered by test_prime(). */
static void prime(lx_mapper_t* mapper, const lx_mapper_sample_t* sample)
{
  lx_mapper_action_t actions[LX_MAPPER_MAX_ACTIONS];
  (void)update(mapper, sample, actions);
}

/* The first update after init is the prime: the full snapshot protocol §5 wants
 * right after the handshake. */
static void test_prime(void)
{
  const lx_mapper_action_t expected[] = {LX_ACTION_INFO, LX_ACTION_PLAY, LX_ACTION_FULLSCREEN};
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 15450, 1000);
  expect("the prime pushes set_info + set_play + set_fullscreen", &mapper, &sample, expected, 3);

  lx_mapper_init(&mapper);
  sample = playing_sample("/music/song.mp3", 15450, 2000);
  sample.state = LX_PLAYER_PAUSED;
  const lx_mapper_action_t paused_expected[] = {LX_ACTION_INFO, LX_ACTION_PAUSE,
                                                LX_ACTION_FULLSCREEN};
  expect("a paused player primes with set_pause", &mapper, &sample, paused_expected, 3);

  lx_mapper_init(&mapper);
  sample = playing_sample("/music/song.mp3", 0, 3000);
  sample.fullscreen = true;
  const lx_mapper_action_t fullscreen_expected[] = {LX_ACTION_INFO, LX_ACTION_PLAY,
                                                    LX_ACTION_FULLSCREEN};
  expect("a fullscreen player primes with set_fullscreen true", &mapper, &sample,
         fullscreen_expected, 3);

  lx_mapper_init(&mapper);
  const lx_mapper_sample_t idle = idle_sample(4000);
  const lx_mapper_action_t idle_expected[] = {LX_ACTION_INFO, LX_ACTION_STOP, LX_ACTION_FULLSCREEN};
  expect("an empty player primes with set_info + set_stop", &mapper, &idle, idle_expected, 3);
}

/* Nothing changed: no action at all, however many ticks go by. */
static void test_steady_playback(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 500, 1000);
  prime(&mapper, &sample);

  for (int tick = 1; tick <= 5; tick++) {
    sample = playing_sample("/music/song.mp3", 500 + tick * 500, 1000 + tick * 500);
    expect("a steady playing position emits nothing", &mapper, &sample, NULL, 0);
  }

  /* Jitter inside the tolerance is not a seek either. */
  sample = playing_sample("/music/song.mp3", 3500 - 1800, 4000);
  expect("a position inside the seek tolerance emits nothing", &mapper, &sample, NULL, 0);
}

static void test_metadata_edit(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 2000, 5000);
  prime(&mapper, &sample);

  sample = playing_sample("/music/song.mp3", 2500, 5500);
  snprintf(sample.name, sizeof(sample.name), "Edited title");
  const lx_mapper_action_t expected[] = {LX_ACTION_INFO};
  expect("a metadata edit on the same track pushes set_info only", &mapper, &sample, expected, 1);

  /* The edit must not have re-armed the seek baseline: the position keeps
   * advancing from where it was. */
  sample = playing_sample("/music/song.mp3", 3000, 6000);
  snprintf(sample.name, sizeof(sample.name), "Edited title");
  expect("the timer keeps running after a metadata edit", &mapper, &sample, NULL, 0);
}

static void test_track_change(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 30000, 7000);
  prime(&mapper, &sample);

  sample = playing_sample("/music/next.mp3", 0, 8000);
  const lx_mapper_action_t expected[] = {LX_ACTION_INFO, LX_ACTION_PLAY};
  expect("a track change while playing pushes set_info + set_play", &mapper, &sample, expected, 2);
  check(strcmp(mapper.previous.path, "/music/next.mp3") == 0, "the mapper remembers the new path");

  /* A track change while paused pushes set_info + set_pause. */
  lx_mapper_sample_t paused = playing_sample("/music/song.mp3", 10000, 8500);
  paused.state = LX_PLAYER_PAUSED;
  prime(&mapper, &paused);
  paused = playing_sample("/music/third.mp3", 0, 9000);
  paused.state = LX_PLAYER_PAUSED;
  const lx_mapper_action_t paused_expected[] = {LX_ACTION_INFO, LX_ACTION_PAUSE};
  expect("a track change while paused pushes set_info + set_pause", &mapper, &paused,
         paused_expected, 2);
}

static void test_state_changes(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 4000, 10000);
  prime(&mapper, &sample);

  lx_mapper_sample_t paused = sample;
  paused.state = LX_PLAYER_PAUSED;
  paused.now_ms = 10500;
  const lx_mapper_action_t pause_expected[] = {LX_ACTION_PAUSE};
  expect("pausing pushes set_pause", &mapper, &paused, pause_expected, 1);

  /* Paused ticks emit nothing: the app's timer is stopped, and the resume below
   * carries the position. */
  paused.played_time_ms = 4000;
  paused.now_ms = 11000;
  expect("a paused player emits nothing while it stays paused", &mapper, &paused, NULL, 0);

  lx_mapper_sample_t resumed = paused;
  resumed.state = LX_PLAYER_PLAYING;
  resumed.now_ms = 11500;
  const lx_mapper_action_t play_expected[] = {LX_ACTION_PLAY};
  expect("resuming pushes set_play at the current position", &mapper, &resumed, play_expected, 1);

  lx_mapper_sample_t stopped = resumed;
  stopped.state = LX_PLAYER_STOPPED;
  stopped.now_ms = 12000;
  const lx_mapper_action_t stop_expected[] = {LX_ACTION_STOP};
  expect("stopping pushes set_stop", &mapper, &stopped, stop_expected, 1);

  /* The input disappearing (the playlist ended) is not a second stop and must
   * not push a set_info for a track that no longer exists. */
  const lx_mapper_sample_t gone = idle_sample(12500);
  expect("losing the input emits nothing once stopped", &mapper, &gone, NULL, 0);

  /* A track appearing while paused is reported as a change (its lyrics must be
   * acquired), and a later seek while it stays paused emits nothing: the
   * position is only re-sent when the player actually starts again. */
  lx_mapper_sample_t paused_seek = gone;
  paused_seek.has_input = true;
  paused_seek.state = LX_PLAYER_PAUSED;
  paused_seek.played_time_ms = 90000;
  paused_seek.now_ms = 13000;
  snprintf(paused_seek.path, sizeof(paused_seek.path), "%s", "/music/song.mp3");
  snprintf(paused_seek.name, sizeof(paused_seek.name), "%s", "Song");
  const lx_mapper_action_t info_pause_expected[] = {LX_ACTION_INFO, LX_ACTION_PAUSE};
  expect("a track appearing while paused pushes set_info + set_pause", &mapper, &paused_seek,
         info_pause_expected, 2);
  paused_seek.now_ms = 13500;
  expect("a paused seek emits nothing else", &mapper, &paused_seek, NULL, 0);
  paused_seek.state = LX_PLAYER_PLAYING;
  paused_seek.now_ms = 14000;
  expect("resuming after a paused seek pushes set_play at the new position", &mapper, &paused_seek,
         play_expected, 1);
}

static void test_seek_heuristic(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 10000, 20000);
  prime(&mapper, &sample);

  /* A forward seek: the position jumped 30 s ahead of the predicted one. */
  lx_mapper_sample_t seeked = playing_sample("/music/song.mp3", 40500, 20500);
  const lx_mapper_action_t expected[] = {LX_ACTION_PLAY};
  expect("a forward seek pushes set_play", &mapper, &seeked, expected, 1);

  /* A backward seek (restart) too. */
  lx_mapper_sample_t restarted = playing_sample("/music/song.mp3", 0, 21000);
  expect("a backward seek pushes set_play", &mapper, &restarted, expected, 1);

  /* Exactly at the tolerance is not a seek; one millisecond past it is. */
  lx_mapper_sample_t edge =
    playing_sample("/music/song.mp3", 500 + LX_MAPPER_SEEK_TOLERANCE_MS, 21500);
  expect("a drift at the tolerance is not a seek", &mapper, &edge, NULL, 0);
  edge = playing_sample("/music/song.mp3",
                        edge.played_time_ms + 500 + LX_MAPPER_SEEK_TOLERANCE_MS + 1, 22000);
  expect("a drift past the tolerance is a seek", &mapper, &edge, expected, 1);

  /* A backwards clock (suspend/resume) is not a seek. */
  lx_mapper_sample_t backwards = playing_sample("/music/song.mp3", edge.played_time_ms + 10, 1000);
  expect("a clock that went backwards is not a seek", &mapper, &backwards, NULL, 0);
}

/* The position advances at `rate`, so a non-1.0 rate must not look like a seek. */
static void test_rate(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 0, 1000);
  sample.rate = 2.0;
  prime(&mapper, &sample);

  for (int tick = 1; tick <= 5; tick++) {
    sample = playing_sample("/music/song.mp3", tick * 1000, 1000 + tick * 500);
    sample.rate = 2.0;
    expect("a doubled rate is not mistaken for a seek", &mapper, &sample, NULL, 0);
  }

  /* The rate itself changing needs no message, but the position must still be
   * checked against the new rate. */
  sample = playing_sample("/music/song.mp3", 6000 + 250, 4000);
  sample.rate = 0.5;
  expect("a rate change with a matching position is not a seek", &mapper, &sample, NULL, 0);

  /* A rate outside the protocol's 0.25-4.0 range falls back to 1.0 instead of
   * poisoning the prediction; a real seek is still detected. */
  const lx_mapper_action_t expected[] = {LX_ACTION_PLAY};
  sample = playing_sample("/music/song.mp3", 60000, 4500);
  sample.rate = 0.0;
  expect("a zero rate falls back to 1.0 and a seek is still detected", &mapper, &sample, expected,
         1);

  sample = playing_sample("/music/song.mp3", 0, 5000);
  sample.rate = 1.0 / 0.0; /* +inf */
  expect("an infinite rate falls back to 1.0", &mapper, &sample, expected, 1);
}

static void test_fullscreen(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  lx_mapper_sample_t sample = playing_sample("/music/song.mp3", 1000, 1000);
  prime(&mapper, &sample);

  sample = playing_sample("/music/song.mp3", 1500, 1500);
  sample.fullscreen = true;
  const lx_mapper_action_t expected[] = {LX_ACTION_FULLSCREEN};
  expect("entering fullscreen pushes set_fullscreen", &mapper, &sample, expected, 1);

  sample = playing_sample("/music/song.mp3", 2000, 2000);
  sample.fullscreen = true;
  expect("staying fullscreen emits nothing", &mapper, &sample, NULL, 0);

  sample = playing_sample("/music/song.mp3", 2500, 2500);
  sample.fullscreen = false;
  expect("leaving fullscreen pushes set_fullscreen", &mapper, &sample, expected, 1);
}

/* A stream has an empty path but a real input: the adapter must still push its
 * (empty) set_info once, and must not confuse it with "nothing playing". */
static void test_stream_input(void)
{
  lx_mapper_t mapper;
  lx_mapper_init(&mapper);
  const lx_mapper_sample_t idle = idle_sample(1000);
  prime(&mapper, &idle);

  lx_mapper_sample_t stream = playing_sample("", 0, 1500);
  const lx_mapper_action_t expected[] = {LX_ACTION_INFO, LX_ACTION_PLAY};
  expect("a stream pushes set_info with an empty path", &mapper, &stream, expected, 2);

  /* Two streams cannot be told apart by an empty path, so a *new* stream is only
   * reported when its (ICY) metadata differs — and then it is a metadata edit,
   * not a track change. */
  stream = playing_sample("", 500, 2000);
  snprintf(stream.name, sizeof(stream.name), "Other station");
  const lx_mapper_action_t info_expected[] = {LX_ACTION_INFO};
  expect("a stream whose metadata changed pushes set_info only", &mapper, &stream, info_expected,
         1);
}

int main(void)
{
  test_prime();
  test_steady_playback();
  test_metadata_edit();
  test_track_change();
  test_state_changes();
  test_seek_heuristic();
  test_rate();
  test_fullscreen();
  test_stream_input();

  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
