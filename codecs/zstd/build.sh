#!/usr/bin/env bash
set -e

ROOT_DIR="node_modules"
rm -rf $ROOT_DIR

CODEC_URL="https://github.com/facebook/zstd"
CODEC_VERSION="v1.5.7"

CODEC_DIR="$ROOT_DIR/zstd"
BUILD_DIR="$CODEC_DIR/build"

export OPTIMIZE="-O3 -flto -msimd128"
export LDFLAGS=$OPTIMIZE
export CFLAGS="$OPTIMIZE -DNDEBUG=1"
export CPPFLAGS=$OPTIMIZE

echo "============================================="
echo "Downloading facebook/zstd"
echo "============================================="

mkdir -p $CODEC_DIR
curl -L "$CODEC_URL/archive/$CODEC_VERSION.tar.gz" | tar -xzf - --strip 1 -C $CODEC_DIR

echo "============================================="
echo "Compiling zstd"
echo "============================================="

cd $BUILD_DIR
(
  emcmake cmake \
    -DZSTD_MULTITHREAD_SUPPORT=OFF \
    -DZSTD_BUILD_PROGRAMS=OFF \
    -DZSTD_BUILD_CONTRIB=OFF \
    -DZSTD_BUILD_TESTS=OFF \
    -DZSTD_BUILD_STATIC=ON \
    -DZSTD_BUILD_SHARED=OFF \
    -DZSTD_LEGACY_SUPPORT=OFF \
    -DCMAKE_C_FLAGS="-DNDEBUG=1" \
    ./cmake
)

cmake --build . -j

echo "============================================="
echo "Compiling wasm bindings"
echo "============================================="

cd ../../../

# Pure C bindings — no embind, no C++, no wasm exceptions.
#
# EXPORTED_FUNCTIONS: low-level C functions + malloc/free for the JS wrapper.
# EXPORTED_RUNTIME_METHODS: HEAPU8/HEAP32 for direct heap access from the
#                           JS wrapper.
# --post-js: injects high-level compress/decompress/free_result wrappers onto
#            the Module object, preserving the same TypeScript interface.
#
# Key optimizations from zstddec-wasm:
#   -DNDEBUG=1: strips all assert() calls from zstd source (significant size win)
#   FILESYSTEM=0: removes emscripten virtual filesystem (large code saving)
#
# See https://emscripten.org/docs/tools_reference/settings_reference.html
(
  emcc zstd_codec.c \
    ${OPTIMIZE} \
    -DNDEBUG=1 \
    --closure 1 \
    --post-js post.js \
    -s EXPORTED_FUNCTIONS='["_get_input_buf","_do_compress","_do_decompress","_free_result","_get_error_msg","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAP32"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s ENVIRONMENT="web" \
    -s MALLOC=emmalloc \
    -s FILESYSTEM=0 \
    -s EXPORT_NAME="zstd_codec" \
    -I "$CODEC_DIR/lib" \
    -lzstd \
    -L "$BUILD_DIR/lib" \
    -o "zstd_codec.js"
)

echo "============================================="
echo "Finished."
echo "============================================="
