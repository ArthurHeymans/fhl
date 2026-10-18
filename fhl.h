/* SPDX-License-Identifier: MIT */
#ifndef FHL_H
#define FHL_H

#include <stddef.h>
#include <stdint.h>

/* Bytes of 8-byte aligned work memory fhl_decode() needs for its tables. */
#define FHL_WORK_SIZE (45 * 1024)

/*
 * Decompress the FHL stream src[0..n) into dst[0..cap). Returns the
 * decompressed size, or 0 if the header is invalid or the output does not
 * fit. The whole output buffer is used during decoding (literals are staged
 * in its tail), so FHL cannot decompress in place.
 *
 * The decoder trusts the stream beyond its header: feed it only streams you
 * produced or verified (e.g. by hash), as with any unchecked LZ decoder.
 */
size_t fhl_decode(const void *src, size_t n, void *dst, size_t cap, void *work);

/* Encoder-side parser used by ANZ; see fhl.c. */
struct fhl_seq { uint32_t L, M, ds, d; };
size_t fhl_parse(const uint8_t *in, uint32_t n, unsigned K, unsigned Kf, struct fhl_seq *out);

#endif
