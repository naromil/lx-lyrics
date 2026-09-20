/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * Audacious analyser source: registers a Visualizer with the host and turns the
 * newest 256-bin FFT frame into the 128 log-spaced bytes docs/protocol.md §5
 * requires.
 *
 * This header is C++ (unlike the DeaDBeeF adapter's) because the host's
 * analyser API is a C++ class — libaudcore/visualizer.h:25
 * `class LIBAUDCORE_PUBLIC Visualizer` — and registering one is public API:
 * libaudcore/interface.h:53 declares `void aud_visualizer_add (Visualizer *)`.
 */

#ifndef LX_SPECTRUM_H
#define LX_SPECTRUM_H

#include "feed.h"

#include <libaudcore/visualizer.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* libaudcore's FFT is 512 samples wide and render_freq() is handed the 256
 * positive frequencies (libaudcore/fft.cc:101-104, visualizer.h:38). */
#define LX_SPECTRUM_BINS 256
/* docs/protocol.md §5: 255 * log10(1 + magnitude * 255) / log10(256). */
#define LX_SPECTRUM_SCALE 255.0
#define LX_SPECTRUM_LOG_BASE 256.0
/* Band edges span 40 Hz to Nyquist, the same range the DeaDBeeF adapter uses. */
#define LX_SPECTRUM_MIN_HZ 40.0

class LxSpectrum : public Visualizer {
public:
  LxSpectrum();
  ~LxSpectrum();

  /** Registers the analyser with the host (`aud_visualizer_add`). Main thread only. */
  void start();
  /** Unregisters it (`aud_visualizer_remove`). Main thread only. */
  void stop();

  /**
   * Sample rate of the current audio stream, which fixes the band edges. Called
   * from the plugin's main-thread snapshot refresh (there is no sample rate in
   * the render_freq() frame itself).
   */
  void set_samplerate(int samplerate);

  /**
   * Downsamples the newest frame into @p out (LX_FEED_SPECTRUM_BYTES bytes):
   * 128 log-spaced bands from LX_SPECTRUM_MIN_HZ to Nyquist, then the §5 log
   * conversion. Called from the feed's reader thread; returns false while no
   * frame (or no sample rate) is available yet.
   */
  bool read(uint8_t* out, size_t size);

  /* Visualizer. Both run on the main thread: the host calls them from its 30 Hz
   * vis timer (libaudcore/vis-runner.cc:132-135 -> visualization.cc:105-106). */
  void clear() override;
  void render_freq(const float* freq) override;

private:
  pthread_mutex_t m_lock;
  float m_bins[LX_SPECTRUM_BINS];
  bool m_have_frame;
  int m_samplerate;
  bool m_registered;
};

#endif /* LX_SPECTRUM_H */
