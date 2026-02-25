#!/usr/bin/env bash
set -e

ROOT_DIR="node_modules"
rm -rf $ROOT_DIR

CODEC_URL="https://github.com/lz4/lz4"
CODEC_VERSION="v1.10.0"

CODEC_DIR="$ROOT_DIR/lz4"

export OPTIMIZE="-O3 -flto -msimd128"
export LDFLAGS=$OPTIMIZE
export CFLAGS=$OPTIMIZE
export CPPFLAGS=$OPTIMIZE

echo "============================================="
echo "Downloading lz4"
echo "============================================="

mkdir -p $CODEC_DIR
curl -L "$CODEC_URL/archive/$CODEC_VERSION.tar.gz" | tar -xzf - --strip 1 -C $CODEC_DIR

echo "============================================="
echo "Compiling lz4"
echo "============================================="

# Build only the static library (not programs/tests), in parallel.
emmake make -C $CODEC_DIR/lib -j

echo "============================================="
echo "Compiling wasm bindings"
echo "============================================="

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
  emcc lz4_codec.c \
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
    -s EXPORT_NAME="lz4_codec" \
    -I "$CODEC_DIR/lib" \
    -llz4 \
    -L "$CODEC_DIR/lib" \
    -o "lz4_codec.js"
)

echo "============================================="
echo "Finished."
echo "============================================="
