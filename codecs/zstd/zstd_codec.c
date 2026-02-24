#include <emscripten.h>
#include "zstd.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/*
 * Lightweight zstd wasm bindings with persistent, reusable buffers.
 *
 * Input and output buffers use grow-only semantics: they are only
 * reallocated when a larger size is needed, eliminating per-call
 * allocation overhead for typical usage patterns.
 *
 * The JS wrapper (post.js) provides the high-level API that matches
 * the ZstdModule interface (compress, decompress, free_result).
 *
 * Decompression uses the streaming API (ZSTD_DStream) so that it can
 * handle frames with unknown content size (produced by streaming
 * compression) and multiple concatenated frames.
 *
 * Error reporting uses a static buffer instead of C++ exceptions,
 * eliminating the need for -fwasm-exceptions and getExceptionMessage.
 */

static uint8_t *input_buf  = NULL;
static size_t   input_cap  = 0;

static uint8_t *output_buf = NULL;
static size_t   output_cap = 0;

/* Static error message buffer. Empty string means no error. */
static char error_msg[256] = "";

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

/* Return the last error message (empty string if no error). */
EMSCRIPTEN_KEEPALIVE
const char *get_error_msg(void) {
	return error_msg;
}

/*
 * Compress data in input_buf (nbytes long) at the given zstd level.
 *
 * On success, writes to output_buf and stores total output size in
 * *out_size. Returns output_buf pointer.
 * On failure, returns NULL, sets *out_size to -1, and writes an
 * error message to error_msg.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_compress(int nbytes, int level, int *out_size) {
	error_msg[0] = '\0';

	size_t dest_cap = ZSTD_compressBound((size_t)nbytes);

	uint8_t *out = ensure_output(dest_cap);
	if (!out) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: cannot allocate output buffer");
		*out_size = -1;
		return NULL;
	}

	size_t compressed = ZSTD_compress(out, dest_cap, input_buf, (size_t)nbytes, level);

	if (ZSTD_isError(compressed)) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: %s", ZSTD_getErrorName(compressed));
		*out_size = -1;
		return NULL;
	}

	*out_size = (int)compressed;
	return out;
}

/*
 * Decompress data in input_buf (nbytes long).
 *
 * Uses the streaming API (ZSTD_DStream) to handle:
 *   - Frames with unknown content size (streaming-compressed data)
 *   - Multiple concatenated frames
 *   - Large outputs that exceed initial buffer guesses
 *
 * On success, writes decompressed data to output_buf and stores
 * the decompressed size in *out_size. Returns output_buf pointer.
 * On failure, returns NULL and writes an error message to error_msg.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_decompress(int nbytes, int *out_size) {
	error_msg[0] = '\0';

	/* Growth increment for output buffer when more space is needed */
	const size_t DEST_GROWTH_SIZE = ZSTD_DStreamOutSize();

	/* Create and initialize decompression stream */
	ZSTD_DStream *zds = ZSTD_createDStream();
	if (!zds) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: cannot create decompression stream");
		*out_size = -1;
		return NULL;
	}

	size_t status = ZSTD_initDStream(zds);
	if (ZSTD_isError(status)) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: %s", ZSTD_getErrorName(status));
		ZSTD_freeDStream(zds);
		*out_size = -1;
		return NULL;
	}

	/* Determine initial output buffer size */
	unsigned long long dest_size = ZSTD_getFrameContentSize(input_buf, (size_t)nbytes);

	if (dest_size == ZSTD_CONTENTSIZE_UNKNOWN) {
		/* Guess: 2x source, minimum DEST_GROWTH_SIZE (~128 KiB) */
		dest_size = (unsigned long long)nbytes * 2;
		if (dest_size < DEST_GROWTH_SIZE)
			dest_size = DEST_GROWTH_SIZE;
	} else if (dest_size == ZSTD_CONTENTSIZE_ERROR) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: content size error");
		ZSTD_freeDStream(zds);
		*out_size = -1;
		return NULL;
	}

	/* Allocate output — we can't use ensure_output here because the
	   streaming loop may need to realloc, and ensure_output uses
	   free+malloc (grow-only). Instead, manage a local pointer and
	   assign to output_buf at the end on success. */
	uint8_t *dest = (uint8_t *)malloc((size_t)dest_size);
	if (!dest) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: cannot allocate output buffer");
		ZSTD_freeDStream(zds);
		*out_size = -1;
		return NULL;
	}

	ZSTD_inBuffer input = { input_buf, (size_t)nbytes, 0 };
	ZSTD_outBuffer output = { dest, (size_t)dest_size, 0 };

	/* Decompress all frames */
	do {
		status = ZSTD_decompressStream(zds, &output, &input);

		if (ZSTD_isError(status)) {
			snprintf(error_msg, sizeof(error_msg),
			         "zstd codec error: %s", ZSTD_getErrorName(status));
			ZSTD_freeDStream(zds);
			free(dest);
			*out_size = -1;
			return NULL;
		}

		/* If there's more data but the output buffer is full, grow it */
		if (status > 0 && output.pos == output.size) {
			size_t new_size = output.size + DEST_GROWTH_SIZE;

			if (new_size < output.size || new_size < DEST_GROWTH_SIZE) {
				/* Overflow */
				snprintf(error_msg, sizeof(error_msg),
				         "zstd codec error: output buffer overflow");
				ZSTD_freeDStream(zds);
				free(dest);
				*out_size = -1;
				return NULL;
			}

			uint8_t *new_dest = (uint8_t *)realloc(dest, new_size);
			if (!new_dest) {
				snprintf(error_msg, sizeof(error_msg),
				         "zstd codec error: could not expand output buffer");
				ZSTD_freeDStream(zds);
				free(dest);
				*out_size = -1;
				return NULL;
			}

			dest = new_dest;
			output.dst = dest;
			output.size = new_size;
		}

	/* status > 0: more bytes in this frame
	   status == 0 && input.pos < input.size: additional frame(s) */
	} while (status > 0 || input.pos < input.size);

	ZSTD_freeDStream(zds);

	/* Transfer the decompressed buffer to the persistent output slot.
	   Free the old output_buf if it's a different allocation. */
	if (output_buf != dest) {
		free(output_buf);
		output_buf = dest;
		output_cap = output.size;
	}

	*out_size = (int)output.pos;
	return output_buf;
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
