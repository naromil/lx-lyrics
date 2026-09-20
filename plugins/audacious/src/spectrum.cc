/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 */

#include "spectrum.h"

#include <libaudcore/interface.h>

#include <math.h>
#include <string.h>

LxSpectrum::LxSpectrum()
  : Visualizer(Visualizer::Freq)
  , m_have_frame(false)
  , m_samplerate(0)
  , m_registered(false)
{
  memset(m_bins, 0, sizeof(m_bins));
  pthread_mutex_init(&m_lock, nullptr);
}

LxSpectrum::~LxSpectrum()
{
  stop();
  pthread_mutex_destroy(&m_lock);
}

void LxSpectrum::start()
{
  if (m_registered) {
    return;
  }
  /* The host hands every registered visualizer the same frames
   * (visualization.cc:97-107); the first registration also switches its vis
   * runner on (visualization.cc:34-41 -> vis-runner.cc:199-205). */
  aud_visualizer_add(this);
  m_registered = true;
}

void LxSpectrum::stop()
{
  if (!m_registered) {
    return;
  }
  aud_visualizer_remove(this);
  m_registered = false;
}

void LxSpectrum::clear()
{
  pthread_mutex_lock(&m_lock);
  memset(m_bins, 0, sizeof(m_bins));
  m_have_frame = false;
  pthread_mutex_unlock(&m_lock);
}

/* Runs on the main thread, inside the host's vis timer. The frame pointer is
 * only valid for the duration of the call, so it is copied out and the maths is
 * left to read() on the feed's reader thread. */
void LxSpectrum::render_freq(const float* freq)
{
  if (!freq) {
    return;
  }
  pthread_mutex_lock(&m_lock);
  memcpy(m_bins, freq, sizeof(m_bins));
  m_have_frame = true;
  pthread_mutex_unlock(&m_lock);
}

void LxSpectrum::set_samplerate(int samplerate)
{
  pthread_mutex_lock(&m_lock);
  m_samplerate = samplerate;
  pthread_mutex_unlock(&m_lock);
}

/* Bin i of a 512-sample FFT covers ~(i + 1) * samplerate / (2 * 256) Hz
 * (fft.cc:101-104: "intensity of frequencies 1/512 ... 256/512 of sample
 * rate"). */
static void lx_spectrum_downsample(const float* bins, int samplerate, uint8_t* out)
{
  const double nyquist = (double)samplerate / 2.0;
  const double bin_hz = (double)samplerate / (2.0 * (double)LX_SPECTRUM_BINS);
  const double ratio = pow(nyquist / LX_SPECTRUM_MIN_HZ, 1.0 / (double)LX_FEED_SPECTRUM_BYTES);
  const double log_base = log10(LX_SPECTRUM_LOG_BASE);

  for (int band = 0; band < LX_FEED_SPECTRUM_BYTES; band++) {
    const double low = LX_SPECTRUM_MIN_HZ * pow(ratio, (double)band);
    const double high = LX_SPECTRUM_MIN_HZ * pow(ratio, (double)band + 1.0);
    int first = (int)ceil(low / bin_hz) - 1;
    int last = (int)ceil(high / bin_hz) - 2;
    if (first < 0) {
      first = 0;
    }
    if (last > LX_SPECTRUM_BINS - 1) {
      last = LX_SPECTRUM_BINS - 1;
    }
    double magnitude;
    if (last < first) {
      /* Narrower than the FFT resolution (the low bands at 256 bins): take the
       * nearest bin instead of leaving a hole in the visualisation. */
      int nearest = (int)llround(low / bin_hz) - 1;
      if (nearest < 0) {
        nearest = 0;
      }
      if (nearest > LX_SPECTRUM_BINS - 1) {
        nearest = LX_SPECTRUM_BINS - 1;
      }
      magnitude = bins[nearest];
    } else {
      double sum = 0.0;
      for (int bin = first; bin <= last; bin++) {
        sum += bins[bin];
      }
      magnitude = sum / (double)(last - first + 1);
    }
    const double scaled = 255.0 * log10(1.0 + magnitude * LX_SPECTRUM_SCALE) / log_base;
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

bool LxSpectrum::read(uint8_t* out, size_t size)
{
  if (!out || size < LX_FEED_SPECTRUM_BYTES) {
    return false;
  }
  float bins[LX_SPECTRUM_BINS];
  pthread_mutex_lock(&m_lock);
  const bool ready = m_have_frame && m_samplerate > 0;
  const int samplerate = m_samplerate;
  memcpy(bins, m_bins, sizeof(bins));
  pthread_mutex_unlock(&m_lock);
  if (!ready) {
    return false;
  }
  lx_spectrum_downsample(bins, samplerate, out);
  return true;
}
