#!/usr/bin/env bash
set -e

ROOT_DIR="node_modules"
rm -rf $ROOT_DIR

CODEC_URL="https://github.com/Blosc/c-blosc"
CODEC_VERSION="v1.21.6"

SNAPPY_URL="https://github.com/andikleen/snappy-c"
SNAPPY_VERSION="master"

BLOSC_DIR="$ROOT_DIR/c-blosc"
BUILD_DIR="$BLOSC_DIR/build"
SNAPPY_DIR="$ROOT_DIR/snappy-c"

export OPTIMIZE="-O3 -flto -msimd128"
export LDFLAGS=$OPTIMIZE
export CFLAGS=$OPTIMIZE
export CPPFLAGS=$OPTIMIZE

echo "============================================="
echo "Downloading c-blosc"
echo "============================================="

mkdir -p $BLOSC_DIR
curl -L "$CODEC_URL/archive/$CODEC_VERSION.tar.gz" | tar -xzf - --strip 1 -C $BLOSC_DIR
# Add missing headers in vendored zlib.
for file in "gzlib.c" "gzread.c" "gzwrite.c"; do \
  sed -i "1s/^/#include <unistd.h>/" "$BLOSC_DIR/internal-complibs/zlib-1.3.1/$file" ; \
done

echo "============================================="
echo "Downloading snappy-c"
echo "============================================="

mkdir -p $SNAPPY_DIR
curl -L "$SNAPPY_URL/archive/$SNAPPY_VERSION.tar.gz" | tar -xzf - --strip 1 -C $SNAPPY_DIR

echo "============================================="
echo "Compiling snappy-c compatibility library"
echo "============================================="

# Build a Google-compatible snappy library from andikleen/snappy-c.
#
# andikleen's API differs from the Google snappy-c.h interface that c-blosc
# expects (different function signatures), so we compile a shim (snappy-c-shim.c)
# that adapts one to the other.
#
# To avoid symbol collisions between andikleen's snappy_compress/snappy_uncompress
# and the Google-compatible versions, we rename andikleen's exports at compile time.
emcc $OPTIMIZE -DNDEBUG -c "$SNAPPY_DIR/snappy.c" -I "$SNAPPY_DIR" \
    -Dsnappy_compress=snappy_compress_ak \
    -Dsnappy_uncompress=snappy_uncompress_ak \
    -Dsnappy_init_env=snappy_init_env_ak \
    -Dsnappy_init_env_sg=snappy_init_env_sg_ak \
    -Dsnappy_free_env=snappy_free_env_ak \
    -o "$SNAPPY_DIR/snappy.o"

emcc $OPTIMIZE -DNDEBUG -c snappy-c-shim.c -I "$SNAPPY_DIR" \
    -o "$SNAPPY_DIR/snappy-c-shim.o"

emar rcs "$SNAPPY_DIR/libsnappy.a" "$SNAPPY_DIR/snappy.o" "$SNAPPY_DIR/snappy-c-shim.o"

# Create the snappy-c.h header that c-blosc expects.
cat > "$SNAPPY_DIR/snappy-c.h" << 'SNAPPY_HDR'
#ifndef SNAPPY_C_COMPAT_H
#define SNAPPY_C_COMPAT_H

#include <stddef.h>

typedef enum {
    SNAPPY_OK = 0,
    SNAPPY_INVALID_INPUT = 1,
    SNAPPY_BUFFER_TOO_SMALL = 2
} snappy_status;

#define SNAPPY_VERSION 1
#define SNAPPY_MAJOR 1
#define SNAPPY_MINOR 1
#define SNAPPY_PATCHLEVEL 0

snappy_status snappy_compress(const char *input, size_t input_length,
                              char *compressed, size_t *compressed_length);
snappy_status snappy_uncompress(const char *compressed, size_t compressed_length,
                                char *uncompressed, size_t *uncompressed_length);
size_t snappy_max_compressed_length(size_t source_length);

#endif
SNAPPY_HDR

echo "============================================="
echo "Compiling blosc"
echo "============================================="

SNAPPY_ABS_DIR="$(cd "$SNAPPY_DIR" && pwd)"

# Replace c-blosc's own FindSnappy.cmake with one that points to our
# pre-compiled andikleen/snappy-c library. c-blosc's CMakeLists.txt
# hardcodes CMAKE_MODULE_PATH to its own cmake/ directory, so we must
# write our FindSnappy.cmake there.
cat > "$BLOSC_DIR/cmake/FindSnappy.cmake" << FINDSNAPPY
set(SNAPPY_FOUND TRUE)
set(SNAPPY_LIBRARY "$SNAPPY_ABS_DIR/libsnappy.a")
set(SNAPPY_INCLUDE_DIR "$SNAPPY_ABS_DIR")
FINDSNAPPY

mkdir $BUILD_DIR
cd $BUILD_DIR
# AVX2 and SSE2 are not supported in WebAssembly.
# Snappy is provided via our pre-compiled andikleen/snappy-c + compatibility shim.
(
  emcmake cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_BENCHMARKS=0 \
    -DBUILD_FUZZERS=0 \
    -DBUILD_SHARED=0 \
    -DBUILD_TESTS=0 \
    -DDEACTIVATE_SNAPPY=OFF \
    -DPREFER_EXTERNAL_SNAPPY=ON \
    ../
)

cmake --build . --parallel

echo "============================================="
echo "Compiling wasm bindings"
echo "============================================="

cd ../../../
# Pure C bindings — no embind, no C++.
#
# EXPORTED_FUNCTIONS: low-level C functions + malloc/free for the JS wrapper.
# EXPORTED_RUNTIME_METHODS: HEAPU8/HEAP32 for direct heap access from the
#                           JS wrapper.
# --post-js: injects high-level compress/decompress/free_result wrappers onto
#            the Module object, preserving the same TypeScript interface.
#
# See https://emscripten.org/docs/tools_reference/settings_reference.html
(
  emcc blosc_codec.c \
    ${OPTIMIZE} \
    -DNDEBUG=1 \
    --closure 1 \
    --post-js post.js \
    -s EXPORTED_FUNCTIONS='["_get_input_buf","_do_compress","_do_decompress","_free_result","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAP32"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s ENVIRONMENT="web" \
    -s MALLOC=emmalloc \
    -s FILESYSTEM=0 \
    -s INITIAL_MEMORY=4194304 \
    -s STACK_SIZE=262144 \
    -s EXPORT_NAME="blosc_codec" \
    -I "$BLOSC_DIR/blosc" \
    -lblosc \
    -L "$BLOSC_DIR/build/blosc" \
    "$SNAPPY_DIR/libsnappy.a" \
    -o "blosc_codec.js"
)

echo "============================================="
echo "Finished."
echo "============================================="
