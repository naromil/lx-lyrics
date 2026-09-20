/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 */

#include "json.h"

#include <string.h>

static int lx_json_escape_needed(unsigned char c)
{
  return c < 0x20 || c == '"' || c == '\\';
}

/* Writes the escape sequence for @p c into @p dst (which needs 6 bytes) and
 * returns its length. Only called for bytes that lx_json_escape_needed() flags. */
static size_t lx_json_escape_one(unsigned char c, char* dst)
{
  static const char hex[] = "0123456789abcdef";
  switch (c) {
  case '"':
    dst[0] = '\\';
    dst[1] = '"';
    return 2;
  case '\\':
    dst[0] = '\\';
    dst[1] = '\\';
    return 2;
  case '\b':
    dst[0] = '\\';
    dst[1] = 'b';
    return 2;
  case '\f':
    dst[0] = '\\';
    dst[1] = 'f';
    return 2;
  case '\n':
    dst[0] = '\\';
    dst[1] = 'n';
    return 2;
  case '\r':
    dst[0] = '\\';
    dst[1] = 'r';
    return 2;
  case '\t':
    dst[0] = '\\';
    dst[1] = 't';
    return 2;
  default:
    dst[0] = '\\';
    dst[1] = 'u';
    dst[2] = '0';
    dst[3] = '0';
    dst[4] = hex[(c >> 4) & 0x0f];
    dst[5] = hex[c & 0x0f];
    return 6;
  }
}

size_t lx_json_escaped_size(const char* s)
{
  size_t size = 1; /* the NUL */
  for (; *s; s++) {
    size += lx_json_escape_needed((unsigned char)*s) ? 6 : 1;
  }
  return size;
}

size_t lx_json_escape(const char* s, char* dst, size_t dst_size)
{
  size_t needed = 0;
  size_t written = 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    char escape[6];
    size_t length = 1;
    if (lx_json_escape_needed(c)) {
      length = lx_json_escape_one(c, escape);
    } else {
      escape[0] = (char)c;
    }
    if (written + length + 1 <= dst_size) {
      memcpy(dst + written, escape, length);
      written += length;
    }
    needed += length;
  }
  if (dst_size > 0) {
    dst[written] = '\0';
  }
  return needed;
}

size_t lx_base64_encode(const uint8_t* data, size_t size, char* out, size_t out_size)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t needed = ((size + 2) / 3) * 4;
  if (out_size < needed + 1) {
    return 0;
  }
  size_t out_len = 0;
  size_t i = 0;
  while (i + 3 <= size) {
    uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
    out[out_len++] = alphabet[(triple >> 18) & 0x3f];
    out[out_len++] = alphabet[(triple >> 12) & 0x3f];
    out[out_len++] = alphabet[(triple >> 6) & 0x3f];
    out[out_len++] = alphabet[triple & 0x3f];
    i += 3;
  }
  if (size - i == 1) {
    uint32_t last = (uint32_t)data[i] << 16;
    out[out_len++] = alphabet[(last >> 18) & 0x3f];
    out[out_len++] = alphabet[(last >> 12) & 0x3f];
    out[out_len++] = '=';
    out[out_len++] = '=';
  } else if (size - i == 2) {
    uint32_t last = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out[out_len++] = alphabet[(last >> 18) & 0x3f];
    out[out_len++] = alphabet[(last >> 12) & 0x3f];
    out[out_len++] = alphabet[(last >> 6) & 0x3f];
    out[out_len++] = '=';
  }
  out[out_len] = '\0';
  return out_len;
}
