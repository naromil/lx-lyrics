/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 */

#define DDB_API_LEVEL 16

#include "spectrum.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The analyser hands out `nframes == fft_size` bins (deadbeef's viz.c uses 4096)
 * laid out planar with a stride of nframes, so the first channel is the first
 * nframes floats. Preallocating for 4096 lets the audio thread copy under a
 * mutex without ever allocating. */
#define LX_SPECTRUM_MAX_BINS 4096
#define LX_SPECTRUM_BANDS 128
#define LX_SPECTRUM_MIN_HZ 40.0
/* docs/protocol.md §5. */
#define LX_SPECTRUM_SCALE 255.0
#define LX_SPECTRUM_LOG_BASE 256.0

struct lx_spectrum {
  DB_functions_t* api;
  pthread_mutex_t lock;
  float bins[LX_SPECTRUM_MAX_BINS];
  int nframes;
  int samplerate;
  bool have_frame;
};

/* Runs on the audio-output thread (deadbeef's viz process queue): copy the
 * frame out and let the feed thread do the maths. */
static void lx_spectrum_callback(void* ctx, const ddb_audio_data_t* data)
{
  struct lx_spectrum* spectrum = ctx;
  if (!data || !data->data || !data->fmt || data->nframes <= 0) {
    return;
  }
  int frames = data->nframes < LX_SPECTRUM_MAX_BINS ? data->nframes : LX_SPECTRUM_MAX_BINS;
  pthread_mutex_lock(&spectrum->lock);
  memcpy(spectrum->bins, data->data, (size_t)frames * sizeof(float));
  spectrum->nframes = frames;
  spectrum->samplerate = data->fmt->samplerate;
  spectrum->have_frame = true;
  pthread_mutex_unlock(&spectrum->lock);
}

lx_spectrum_t* lx_spectrum_start(DB_functions_t* api)
{
  if (!api || !api->vis_spectrum_listen2) {
    return NULL;
  }
  struct lx_spectrum* spectrum = calloc(1, sizeof(*spectrum));
  if (!spectrum) {
    return NULL;
  }
  spectrum->api = api;
  pthread_mutex_init(&spectrum->lock, NULL);
  api->vis_spectrum_listen2(spectrum, lx_spectrum_callback);
  return spectrum;
}

void lx_spectrum_stop(lx_spectrum_t* spectrum)
{
  if (!spectrum) {
    return;
  }
  /* Never call this from the analyser callback: deadbeef dispatches the
   * listener list synchronously on its viz queue. */
  spectrum->api->vis_spectrum_unlisten(spectrum);
  pthread_mutex_destroy(&spectrum->lock);
  free(spectrum);
}

/* Bin i of an nframes FFT covers ~(i + 1) * samplerate / (2 * nframes) Hz. */
static void lx_spectrum_downsample(const float* bins, int nframes, int samplerate, uint8_t* out)
{
  const double nyquist = (double)samplerate / 2.0;
  const double bin_hz = (double)samplerate / (2.0 * (double)nframes);
  const double ratio = pow(nyquist / LX_SPECTRUM_MIN_HZ, 1.0 / (double)LX_SPECTRUM_BANDS);
  const double log_base = log10(LX_SPECTRUM_LOG_BASE);

  for (int band = 0; band < LX_SPECTRUM_BANDS; band++) {
    double low = LX_SPECTRUM_MIN_HZ * pow(ratio, (double)band);
    double high = LX_SPECTRUM_MIN_HZ * pow(ratio, (double)band + 1.0);
    int first = (int)ceil(low / bin_hz) - 1;
    int last = (int)ceil(high / bin_hz) - 2;
    if (first < 0) {
      first = 0;
    }
    if (last > nframes - 1) {
      last = nframes - 1;
    }
    double magnitude;
    if (last < first) {
      /* Narrower than the FFT resolution (the low bands): take the nearest bin
       * instead of leaving a hole in the visualisation. */
      int nearest = (int)llround(low / bin_hz) - 1;
      if (nearest < 0) {
        nearest = 0;
      }
      if (nearest > nframes - 1) {
        nearest = nframes - 1;
      }
      magnitude = bins[nearest];
    } else {
      double sum = 0.0;
      for (int bin = first; bin <= last; bin++) {
        sum += bins[bin];
      }
      magnitude = sum / (double)(last - first + 1);
    }
    double scaled = 255.0 * log10(1.0 + magnitude * LX_SPECTRUM_SCALE) / log_base;
    if (!(scaled > 0.0)) {
      /* Silence, or a magnitude the FFT did not produce: 0, never NaN. */
      out[band] = 0;
      continue;
    }
    long value = lround(scaled);
    if (value < 0) {
      value = 0;
    }
    if (value > 255) {
      value = 255;
    }
    out[band] = (uint8_t)value;
  }
}

bool lx_spectrum_read(lx_spectrum_t* spectrum, uint8_t* out, size_t out_size)
{
  if (!spectrum || out_size < LX_FEED_SPECTRUM_BYTES) {
    return false;
  }
  float bins[LX_SPECTRUM_MAX_BINS];
  pthread_mutex_lock(&spectrum->lock);
  bool ready = spectrum->have_frame;
  int nframes = spectrum->nframes;
  int samplerate = spectrum->samplerate;
  if (ready) {
    memcpy(bins, spectrum->bins, (size_t)nframes * sizeof(float));
  }
  pthread_mutex_unlock(&spectrum->lock);
  if (!ready || nframes <= 0 || samplerate <= 0) {
    return false;
  }
  lx_spectrum_downsample(bins, nframes, samplerate, out);
  return true;
}
