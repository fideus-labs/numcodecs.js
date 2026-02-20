#include <emscripten.h>
#include "lz4.h"
#include <stdlib.h>
#include <stdint.h>

/*
 * Lightweight LZ4 wasm bindings with persistent, reusable buffers.
 *
 * Input and output buffers use grow-only semantics: they are only
 * reallocated when a larger size is needed, eliminating per-call
 * allocation overhead for typical usage patterns.
 *
 * The JS wrapper (post.js) provides the high-level API that matches
 * the LZ4Module interface (compress, decompress, free_result).
 *
 * Compressed format (numcodecs convention):
 *   [4 bytes LE uncompressed size] [LZ4 compressed data]
 */

static uint8_t *input_buf  = NULL;
static size_t   input_cap  = 0;

static uint8_t *output_buf = NULL;
static size_t   output_cap = 0;

static const int HEADER_SIZE = 4; /* 4-byte LE uncompressed size prefix */

/* Store a 32-bit integer as little-endian at `p`. */
static inline void store_le32(uint8_t *p, uint32_t v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

/* Load a 32-bit little-endian integer from `p`. */
static inline uint32_t load_le32(const uint8_t *p) {
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}

/* Ensure input buffer has at least `size` bytes. Returns pointer. */
EMSCRIPTEN_KEEPALIVE
uint8_t *get_input_buf(size_t size) {
	if (size > input_cap) {
		free(input_buf);
		input_buf = (uint8_t *)malloc(size);
		input_cap = input_buf ? size : 0;
	}
	return input_buf;
}

/* Ensure output buffer has at least `size` bytes. Returns pointer. */
static uint8_t *ensure_output(size_t size) {
	if (size > output_cap) {
		free(output_buf);
		output_buf = (uint8_t *)malloc(size);
		output_cap = output_buf ? size : 0;
	}
	return output_buf;
}

/*
 * Compress data in input_buf (nbytes long).
 *
 * Output format: [4 bytes LE original size] [LZ4 compressed data]
 *
 * On success, writes to output_buf and stores total output size
 * (header + compressed) in *out_size. Returns output_buf pointer.
 * On failure, returns NULL and sets *out_size to a negative value.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_compress(int nbytes, int acceleration, int *out_size) {
	int max_compressed = LZ4_compressBound(nbytes);
	size_t dest_cap = (size_t)max_compressed + HEADER_SIZE;

	uint8_t *out = ensure_output(dest_cap);
	if (!out) {
		*out_size = -1;
		return NULL;
	}

	/* Write uncompressed size as 4-byte LE header */
	store_le32(out, (uint32_t)nbytes);

	int compressed = LZ4_compress_fast(
		(const char *)input_buf,
		(char *)(out + HEADER_SIZE),
		nbytes,
		max_compressed,
		acceleration
	);

	if (compressed <= 0) {
		*out_size = -1;
		return NULL;
	}

	*out_size = compressed + HEADER_SIZE;
	return out;
}

/*
 * Decompress data in input_buf.
 * Reads a 4-byte LE header to determine the original uncompressed size.
 * On success, writes decompressed data to output_buf and stores
 * the decompressed size in *out_size. Returns output_buf pointer.
 * On failure, returns NULL.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_decompress(int nbytes, int *out_size) {
	if (nbytes < HEADER_SIZE) {
		*out_size = -1;
		return NULL;
	}

	uint32_t dest_size = load_le32(input_buf);

	uint8_t *out = ensure_output((size_t)dest_size);
	if (!out) {
		*out_size = -1;
		return NULL;
	}

	int decompressed = LZ4_decompress_safe(
		(const char *)(input_buf + HEADER_SIZE),
		(char *)out,
		nbytes - HEADER_SIZE,
		(int)dest_size
	);

	if (decompressed < 0) {
		*out_size = -1;
		return NULL;
	}

	*out_size = decompressed;
	return out;
}

/* Free both persistent buffers and reset state. */
EMSCRIPTEN_KEEPALIVE
void free_result(void) {
	free(input_buf);
	input_buf = NULL;
	input_cap = 0;

	free(output_buf);
	output_buf = NULL;
	output_cap = 0;
}
