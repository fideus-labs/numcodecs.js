#include <emscripten.h>
#include "zstd.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/*
 * Lightweight zstd wasm bindings with persistent, reusable buffers
 * and persistent compression/decompression contexts.
 *
 * Input and output buffers use grow-only semantics: they are only
 * reallocated when a larger size is needed, eliminating per-call
 * allocation overhead for typical usage patterns.
 *
 * Compression and decompression contexts (ZSTD_CCtx / ZSTD_DCtx)
 * are created once on first use and reused across calls via
 * ZSTD_CCtx_reset() / ZSTD_DCtx_reset(), avoiding the overhead
 * of allocating and freeing internal context state on every call.
 *
 * Decompression uses two paths:
 *   - Fast path: ZSTD_decompressDCtx() for single frames with known
 *     content size (the common case).
 *   - Streaming path: ZSTD_decompressStream() for frames with unknown
 *     content size or multiple concatenated frames.
 *
 * The JS wrapper (post.js) provides the high-level API that matches
 * the ZstdModule interface (compress, decompress, free_result).
 *
 * Error reporting uses a static buffer instead of C++ exceptions,
 * eliminating the need for -fwasm-exceptions and getExceptionMessage.
 */

static uint8_t *input_buf  = NULL;
static size_t   input_cap  = 0;

static uint8_t *output_buf = NULL;
static size_t   output_cap = 0;

/* Persistent compression/decompression contexts (created on first use). */
static ZSTD_CCtx *cctx = NULL;
static ZSTD_DCtx *dctx = NULL;

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
 * Uses a persistent ZSTD_CCtx that is created on first call and
 * reused (with session reset) on subsequent calls, avoiding per-call
 * context allocation overhead.
 *
 * On success, writes to output_buf and stores total output size in
 * *out_size. Returns output_buf pointer.
 * On failure, returns NULL, sets *out_size to -1, and writes an
 * error message to error_msg.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_compress(int nbytes, int level, int *out_size) {
	error_msg[0] = '\0';

	/* Create compression context on first use */
	if (!cctx) {
		cctx = ZSTD_createCCtx();
		if (!cctx) {
			snprintf(error_msg, sizeof(error_msg),
			         "zstd codec error: cannot create compression context");
			*out_size = -1;
			return NULL;
		}
	}

	size_t dest_cap = ZSTD_compressBound((size_t)nbytes);

	uint8_t *out = ensure_output(dest_cap);
	if (!out) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: cannot allocate output buffer");
		*out_size = -1;
		return NULL;
	}

	size_t compressed = ZSTD_compressCCtx(cctx, out, dest_cap,
	                                      input_buf, (size_t)nbytes, level);

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
 * Uses a persistent ZSTD_DCtx that is created on first call and
 * reused (with session reset) on subsequent calls.
 *
 * Two decompression paths:
 *   - Fast path: when the frame has a known content size and is a
 *     single frame, uses ZSTD_decompressDCtx() with the persistent
 *     grow-only output buffer. This is the common case.
 *   - Streaming path: when content size is unknown or there are
 *     multiple concatenated frames, uses ZSTD_decompressStream()
 *     with a local buffer that is transferred to the persistent
 *     output slot on success.
 *
 * On success, writes decompressed data to output_buf and stores
 * the decompressed size in *out_size. Returns output_buf pointer.
 * On failure, returns NULL and writes an error message to error_msg.
 */
EMSCRIPTEN_KEEPALIVE
uint8_t *do_decompress(int nbytes, int *out_size) {
	error_msg[0] = '\0';

	/* Create decompression context on first use */
	if (!dctx) {
		dctx = ZSTD_createDCtx();
		if (!dctx) {
			snprintf(error_msg, sizeof(error_msg),
			         "zstd codec error: cannot create decompression context");
			*out_size = -1;
			return NULL;
		}
	}

	/* Determine content size and whether streaming is needed */
	unsigned long long content_size = ZSTD_getFrameContentSize(input_buf, (size_t)nbytes);

	if (content_size == ZSTD_CONTENTSIZE_ERROR) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: content size error");
		*out_size = -1;
		return NULL;
	}

	/*
	 * Fast path: known content size, single frame.
	 * Use ZSTD_decompressDCtx() with the persistent grow-only buffer.
	 * This avoids a local malloc and benefits from buffer reuse.
	 *
	 * We detect single-frame data by checking if findFrameCompressedSize
	 * accounts for the entire input.
	 */
	if (content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
		size_t frame_size = ZSTD_findFrameCompressedSize(input_buf, (size_t)nbytes);
		int single_frame = !ZSTD_isError(frame_size) && frame_size == (size_t)nbytes;

		if (single_frame) {
			uint8_t *out = ensure_output((size_t)content_size);
			if (!out) {
				snprintf(error_msg, sizeof(error_msg),
				         "zstd codec error: cannot allocate output buffer");
				*out_size = -1;
				return NULL;
			}

			size_t decompressed = ZSTD_decompressDCtx(dctx, out,
			                                          (size_t)content_size,
			                                          input_buf,
			                                          (size_t)nbytes);

			if (ZSTD_isError(decompressed)) {
				snprintf(error_msg, sizeof(error_msg),
				         "zstd codec error: %s", ZSTD_getErrorName(decompressed));
				*out_size = -1;
				return NULL;
			}

			*out_size = (int)decompressed;
			return output_buf;
		}
	}

	/*
	 * Streaming path: unknown content size or multiple concatenated frames.
	 * Uses ZSTD_decompressStream() with a local buffer.
	 */

	/* Reset the decompression context for a new streaming session */
	size_t status = ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
	if (ZSTD_isError(status)) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: %s", ZSTD_getErrorName(status));
		*out_size = -1;
		return NULL;
	}

	/* Growth increment for output buffer when more space is needed */
	const size_t DEST_GROWTH_SIZE = ZSTD_DStreamOutSize();

	/* Determine initial output buffer size */
	size_t dest_size;
	if (content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
		/* Known size but multiple frames — use content size as initial guess
		   (it's only the first frame's size, but a reasonable starting point) */
		dest_size = (size_t)content_size;
	} else {
		/* Unknown size: guess 2x source, minimum DEST_GROWTH_SIZE (~128 KiB) */
		dest_size = (size_t)nbytes * 2;
		if (dest_size < DEST_GROWTH_SIZE)
			dest_size = DEST_GROWTH_SIZE;
	}

	/* Allocate output — we use a local pointer because the streaming
	   loop may need to realloc. Transfer to output_buf on success. */
	uint8_t *dest = (uint8_t *)malloc(dest_size);
	if (!dest) {
		snprintf(error_msg, sizeof(error_msg),
		         "zstd codec error: cannot allocate output buffer");
		*out_size = -1;
		return NULL;
	}

	ZSTD_inBuffer input = { input_buf, (size_t)nbytes, 0 };
	ZSTD_outBuffer output = { dest, dest_size, 0 };

	/* Decompress all frames */
	do {
		status = ZSTD_decompressStream(dctx, &output, &input);

		if (ZSTD_isError(status)) {
			snprintf(error_msg, sizeof(error_msg),
			         "zstd codec error: %s", ZSTD_getErrorName(status));
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
				free(dest);
				*out_size = -1;
				return NULL;
			}

			uint8_t *new_dest = (uint8_t *)realloc(dest, new_size);
			if (!new_dest) {
				snprintf(error_msg, sizeof(error_msg),
				         "zstd codec error: could not expand output buffer");
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

/* Free both persistent buffers, contexts, and reset state. */
EMSCRIPTEN_KEEPALIVE
void free_result(void) {
	free(input_buf);
	input_buf = NULL;
	input_cap = 0;

	free(output_buf);
	output_buf = NULL;
	output_cap = 0;

	if (cctx) {
		ZSTD_freeCCtx(cctx);
		cctx = NULL;
	}
	if (dctx) {
		ZSTD_freeDCtx(dctx);
		dctx = NULL;
	}
}
