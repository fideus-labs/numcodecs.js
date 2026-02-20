#!/usr/bin/env bash
set -e

ROOT_DIR="node_modules"
rm -rf $ROOT_DIR

CODEC_URL="https://github.com/Blosc/c-blosc"
CODEC_VERSION="v1.21.6"

BLOSC_DIR="$ROOT_DIR/c-blosc"
BUILD_DIR="$BLOSC_DIR/build"

export OPTIMIZE="-Os -flto"
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
echo "Compiling blosc"
echo "============================================="

mkdir $BUILD_DIR
cd $BUILD_DIR
# AVX2 and SSE2 are not supported in WebAssembly
(
  emcmake cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_BENCHMARKS=0 \
    -DBUILD_FUZZERS=0 \
    -DBUILD_SHARED=0 \
    -DBUILD_TESTS=0 \
    -DDEACTIVATE_AVX2=1 \
    -DDEACTIVATE_SSE2=1 \
    ../
)

cmake --build . --parallel

echo "============================================="
echo "Compiling wasm bindings"
echo "============================================="

cd ../../../
# The -s settings which are specific to emcc can be found in detail at
# https://github.com/emscripten-core/emscripten/blob/master/src/settings.js
#
# - MODULARIZE & EXPORT_ES6 flags generated js glue code as a module.
(
  emcc blosc_codec.cpp \
    ${OPTIMIZE} \
    --closure 1 \
    --bind \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s MALLOC=emmalloc \
    -s EXPORT_NAME="blosc_codec" \
    -x c++ \
    --std=c++17 \
    -I "$BLOSC_DIR/blosc" \
    -lblosc \
    -L "$BLOSC_DIR/build/blosc" \
    -o "blosc_codec.js"
)

echo "============================================="
echo "Finished."
echo "============================================="
