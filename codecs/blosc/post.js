// High-level wrappers injected into the Module via --post-js.
// These provide the same API as the previous embind-based bindings:
//   Module.compress(data, cname, clevel, shuffle, blocksize) -> Uint8Array (view)
//   Module.decompress(data) -> Uint8Array (view)
//   Module.free_result() -> void
//
// The returned Uint8Array is a VIEW into wasm memory. Callers must copy it
// (e.g. `new Uint8Array(view)`) before any subsequent wasm calls or
// free_result(), as the underlying buffer may be reused or freed.

// Persistent 4-byte slot for out_size parameter (allocated on first use).
var _outSizePtr = 0;
function _getOutSizePtr() {
  if (!_outSizePtr) _outSizePtr = Module['_malloc'](4);
  return _outSizePtr;
}

Module['compress'] = function (data, cname, clevel, shuffle, blocksize) {
  var bytes =
    data instanceof Uint8Array
      ? data
      : new Uint8Array(data.buffer ? data.buffer : data, data.byteOffset || 0, data.byteLength || data.length);
  var n = bytes.length;

  // Copy input into persistent wasm buffer
  var inputPtr = Module['_get_input_buf'](n);
  if (!inputPtr) throw new Error('Blosc: failed to allocate input buffer');
  Module['HEAPU8'].set(bytes, inputPtr);

  // Write cname as null-terminated ASCII string on the heap
  var cnameLen = cname.length;
  var cnamePtr = Module['_malloc'](cnameLen + 1);
  for (var i = 0; i < cnameLen; i++) {
    Module['HEAPU8'][cnamePtr + i] = cname.charCodeAt(i);
  }
  Module['HEAPU8'][cnamePtr + cnameLen] = 0;

  var outSizePtr = _getOutSizePtr();
  var outPtr = Module['_do_compress'](n, cnamePtr, clevel, shuffle, blocksize, outSizePtr);
  Module['_free'](cnamePtr);

  // Read the result size (int32 at outSizePtr)
  var ret = Module['HEAP32'][outSizePtr >> 2];

  if (!outPtr || ret <= 0) throw new Error('Blosc compression error: ' + ret);

  return new Uint8Array(Module['HEAPU8'].buffer, outPtr, ret);
};

Module['decompress'] = function (data) {
  var bytes =
    data instanceof Uint8Array
      ? data
      : new Uint8Array(data.buffer ? data.buffer : data, data.byteOffset || 0, data.byteLength || data.length);
  var n = bytes.length;

  // Copy input into persistent wasm buffer
  var inputPtr = Module['_get_input_buf'](n);
  if (!inputPtr) throw new Error('Blosc: failed to allocate input buffer');
  Module['HEAPU8'].set(bytes, inputPtr);

  var outSizePtr = _getOutSizePtr();
  var outPtr = Module['_do_decompress'](outSizePtr);

  // Read the result size (int32 at outSizePtr)
  var ret = Module['HEAP32'][outSizePtr >> 2];

  if (!outPtr || ret <= 0) throw new Error('Blosc decompression error: ' + ret);

  return new Uint8Array(Module['HEAPU8'].buffer, outPtr, ret);
};

Module['free_result'] = function () {
  Module['_free_result']();
};
