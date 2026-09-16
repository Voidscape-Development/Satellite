# Satellite tests

```sh
cmake -S . -B build -DENABLE_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Off by default, so the normal plugin build and the packaging CI are unaffected.

## Why there is a fake NDI runtime

The NDI runtime is proprietary. It cannot be redistributed, cannot be installed in CI, and
is not present on most development machines — so without something standing in for it,
nothing about Satellite's NDI path could be verified anywhere.

`fake-ndi.cpp` is a small shared library that exports `NDIlib_v6_load()` and implements the
handful of entry points Satellite actually calls. Crucially it is **compiled against the same
vendored SDK header** (`lib/ndi/Processing.NDI.DynamicLoad.h`) that the backend compiles
against, so if Satellite's use of the ABI is wrong — a misnamed field, a bad struct
assumption — it fails here rather than on a user's machine.

The test builds it under the name the loader searches for (`libndi.so.6`, or `libndi.dylib`
on macOS), points `NDI_RUNTIME_DIR_V6` at its directory, and then exercises the **real**
loader. Nothing about the load path is mocked.

## What `ndi-backend-test` covers

- The runtime is located, `NDIlib_v6_load` binds, and the CPU/initialize/version calls work
- Discovery returns sources, and their strings are copied out of the finder's buffer
- A receiver is created and reports connection state
- Every pixel format the backend claims to support is converted: UYVY, BGRA, NV12, I420 and
  YV12, checking the OBS format, stride, and plane pointers — including that YV12's V and U
  planes are swapped into OBS's Y, U, V order
- Audio arrives as 32-bit planar float with the channel stride laid out as OBS expects
- Frame and dropped-frame counters are read back from the runtime, and NDI's bitrate is
  flagged estimated (the runtime exposes no byte counters)
- Receiver and backend tear down cleanly

## What it does not cover

It is a fake, so it proves Satellite drives the ABI correctly — not that the real runtime
behaves the way the fake does. Timing, reconnection, genuine network behaviour and real
sender interop still need a machine with NDI installed.
