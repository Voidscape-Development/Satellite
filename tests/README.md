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

## What the tests cover

### `ndi-backend-test` — the receive path

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

### `sender-test` — the send path

- A sender is created and the **bare** name reaches NDI, because the `MACHINE (name)` prefix
  people expect is added by the runtime rather than by us
- Video reaches the ABI as UYVY with its resolution, stride and 100ns timecode intact
- Audio goes out as FLTP with the right channel count, sample count, rate and channel stride
- Tally and connection count are read back, and the sender is destroyed on release
- `SenderSession`'s queue, in both regimes:
  - **Sink keeping up** — every frame is forwarded and nothing is dropped
  - **Sink too slow** (`FAKE_NDI_SEND_DELAY_MS` makes the fake stall) — video is dropped
    rather than buffered into a growing backlog, the drops are counted for the dock, and
    **audio is never dropped**, because a gap in sound is far more noticeable than a
    skipped frame
- Frames are copied out of the caller's buffer, so what the send thread emits does not
  depend on memory the pusher has since reused
- The feed appears in the registry as a sender while running and is gone after stop

## What it does not cover

It is a fake, so it proves Satellite drives the ABI correctly — not that the real runtime
behaves the way the fake does. Timing, reconnection, genuine network behaviour and real
sender interop still need a machine with NDI installed.
