/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * DeaDBeeF analyser source: subscribes to the host's FFT data and turns the
 * newest frame into the 128 log-spaced bytes docs/protocol.md §5 requires.
 */

#ifndef LX_SPECTRUM_H
#define LX_SPECTRUM_H

#include "feed.h"

#include <deadbeef/deadbeef.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lx_spectrum lx_spectrum_t;

/**
 * Registers the analyser listener (`vis_spectrum_listen2`, API 1.15). Returns
 * NULL when the host exposes no analyser or on allocation failure: the caller
 * then declares `spectrum:false` in the hello line.
 */
lx_spectrum_t* lx_spectrum_start(DB_functions_t* api);

/** Unregisters the listener and releases the buffer. NULL is a no-op. */
void lx_spectrum_stop(lx_spectrum_t* spectrum);

/**
 * Downsamples the newest frame into @p out (LX_FEED_SPECTRUM_BYTES bytes):
 * first channel, band-averaged into 128 log-spaced bands from 40 Hz to Nyquist,
 * then the §5 log conversion. Returns false while no frame has arrived yet.
 */
bool lx_spectrum_read(lx_spectrum_t* spectrum, uint8_t* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* LX_SPECTRUM_H */
