#include <emscripten.h>
#include "blosc.h"
#include <stdlib.h>
#include <stdint.h>

/*
 * Lightweight blosc wasm bindings with persistent, reusable buffers.
 *
 * Input and output buffers use grow-only semantics: they are only
 * reallocated when a larger size is needed, eliminating per-call
 * allocation overhead for typical usage patterns.
 *
 * The JS wrapper (post.js) provides the high-level API that matches
 * the BloscModule interface (compress, decompress, free_result).
 */

static uint8_t *input_buf  = NULL;
static size_t   input_cap  = 0;

static uint8_t *output_buf = NULL;
static size_t   output_cap = 0;

/* Ensure input buffer has at least `size` bytes. Returns pointer.
   Uses realloc so the allocator can extend in-place when possible,
   avoiding heap fragmentation and unnecessary memory.grow() calls. */
EMSCRIPTEN_KEEPALIVE
uint8_t *get_input_buf(size_t size) {
	if (size > input_cap) {
		uint8_t *p = (uint8_t *)realloc(input_buf, size);
		if (p) {
			input_buf = p;
			input_cap = size;
		} else {
			free(input_buf);
			input_buf = NULL;
			input_cap = 0;
		}
	}
	return input_buf;
}

/* Ensure output buffer has at least `size` bytes. Returns pointer.
   Uses realloc so the allocator can extend in-place when possible. */
static uint8_t *ensure_output(size_t size) {
	if (size > output_cap) {
		uint8_t *p = (uint8_t *)realloc(output_buf, size);
		if (p) {
			output_buf = p;
			output_cap = size;
		} else {
			free(output_buf);
			output_buf = NULL;
			output_cap = 0;
		}
	}
	return output_buf;
}

/*
 * Compress data in input_buf (nbytes long).
 * On success, writes compressed data to output_buf and stores
 * the compressed size in *out_size. Returns output_buf pointer.
 * On failure, returns NULL.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_compress(size_t nbytes, const char *cname, int clevel,
                     int shuffle, int blocksize, int *out_size) {
	/* Validate that the compressor is available to avoid a wasm trap
	   from calling through a null function pointer. */
	if (blosc_compname_to_compcode(cname) < 0) {
		*out_size = -2;
		return NULL;
	}

	size_t dest_size = nbytes + BLOSC_MAX_OVERHEAD;
	uint8_t *out = ensure_output(dest_size);
	if (!out) {
		*out_size = -1;
		return NULL;
	}

	int ret = blosc_compress_ctx(
		clevel,
		shuffle,
		sizeof(int),       /* typesize */
		nbytes,
		input_buf,
		out,
		dest_size,
		cname,
		(size_t)blocksize,
		1                  /* nthreads */
	);

	*out_size = ret;
	return (ret > 0) ? out : NULL;
}

/*
 * Decompress data in input_buf.
 * Reads blosc header to determine output size.
 * On success, writes decompressed data to output_buf and stores
 * the decompressed size in *out_size. Returns output_buf pointer.
 * On failure, returns NULL.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_decompress(int *out_size) {
	size_t uncompressed, cbytes, blocksize;
	blosc_cbuffer_sizes(input_buf, &uncompressed, &cbytes, &blocksize);

	uint8_t *out = ensure_output(uncompressed);
	if (!out) {
		*out_size = -1;
		return NULL;
	}

	int ret = blosc_decompress_ctx(
		input_buf,
		out,
		uncompressed,
		1                  /* nthreads */
	);

	*out_size = ret;
	return (ret > 0) ? out : NULL;
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
