/*
 * Compatibility shim: adapts the andikleen/snappy-c pure-C library
 * to the Google snappy-c.h interface expected by c-blosc.
 *
 * The andikleen snappy functions are compiled with renamed symbols
 * (suffixed _ak) to avoid collisions with the Google-compatible
 * names we export here.
 */
#include <stddef.h>
#include <stdbool.h>

/* ---- Forward declarations of renamed andikleen functions ---- */

struct snappy_env {
	unsigned short *hash_table;
	void *scratch;
	void *scratch_output;
};

int snappy_init_env_ak(struct snappy_env *env);
int snappy_compress_ak(struct snappy_env *env,
                       const char *input, size_t input_length,
                       char *compressed, size_t *compressed_length);
int snappy_uncompress_ak(const char *compressed, size_t n, char *uncompressed);

/* From andikleen's snappy.h — these are NOT renamed because they don't
   collide with the Google API. */
extern bool snappy_uncompressed_length(const char *buf, size_t len, size_t *result);
extern size_t snappy_max_compressed_length(size_t source_len);

/* ---- Google snappy-c.h compatible API ---- */

typedef enum {
	SNAPPY_OK = 0,
	SNAPPY_INVALID_INPUT = 1,
	SNAPPY_BUFFER_TOO_SMALL = 2
} snappy_status;

/* Single static env, initialized on first use. */
static struct snappy_env g_env;
static int g_env_inited = 0;

static void ensure_env(void) {
	if (!g_env_inited) {
		snappy_init_env_ak(&g_env);
		g_env_inited = 1;
	}
}

snappy_status snappy_compress(const char *input, size_t input_length,
                              char *compressed, size_t *compressed_length) {
	ensure_env();
	int ret = snappy_compress_ak(&g_env, input, input_length,
	                             compressed, compressed_length);
	return (ret == 0) ? SNAPPY_OK : SNAPPY_INVALID_INPUT;
}

snappy_status snappy_uncompress(const char *compressed, size_t compressed_length,
                                char *uncompressed, size_t *uncompressed_length) {
	size_t expected_len = 0;
	if (!snappy_uncompressed_length(compressed, compressed_length, &expected_len)) {
		return SNAPPY_INVALID_INPUT;
	}
	if (*uncompressed_length < expected_len) {
		return SNAPPY_BUFFER_TOO_SMALL;
	}

	int ret = snappy_uncompress_ak(compressed, compressed_length, uncompressed);
	if (ret != 0) {
		return SNAPPY_INVALID_INPUT;
	}
	*uncompressed_length = expected_len;
	return SNAPPY_OK;
}

/* snappy_max_compressed_length has the same signature in both APIs
   and is provided by andikleen's snappy.c (not renamed). */
