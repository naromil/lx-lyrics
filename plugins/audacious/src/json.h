/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * JSON string escaping and base64 encoding for the protocol v2 feed
 * (docs/protocol.md §2/§5). Pure C99, no dependencies.
 */

#ifndef LX_JSON_H
#define LX_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Worst-case length of the escaped body of @p s, including the terminating NUL.
 */
size_t lx_json_escaped_size(const char* s);

/**
 * Escapes @p s as the *body* of a JSON string (no surrounding quotes) into
 * @p dst: `"` and `\` get backslash escapes, control characters use the short
 * escapes where JSON defines them and `\u00xx` otherwise. All other bytes pass
 * through verbatim, so UTF-8 text stays UTF-8 (Audacious hands out UTF-8 tuples
 * and URI strings, and `path` is one of them).
 *
 * Always NUL-terminates when @p dst_size > 0 and returns the length the
 * complete body needs, excluding the NUL (snprintf semantics: a return value
 * >= @p dst_size means the result was truncated).
 */
size_t lx_json_escape(const char* s, char* dst, size_t dst_size);

/**
 * Standard base64 (RFC 4648 §4, with padding) of @p size bytes into @p out.
 * Returns the number of characters written, excluding the NUL, or 0 when
 * @p out_size cannot hold the encoding plus its NUL.
 */
size_t lx_base64_encode(const uint8_t* data, size_t size, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* LX_JSON_H */
