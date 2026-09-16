# Satellite tests

```sh
cmake -S . -B build -DENABLE_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Off by default, so the normal plugin build and the packaging CI are unaffected.

## Why there are fake runtimes

Neither transport library can be installed in CI. The NDI runtime is proprietary and not
redistributable; the OMT libraries are published only as source and need a .NET 8 NativeAOT
build. Without something standing in for them, nothing about either protocol path could be
verified anywhere.

`fake-ndi.cpp` and `fake-omt.cpp` are small shared libraries that export the same symbols the
backends bind, implementing the entry points Satellite actually calls. Crucially each is
**compiled against the same vendored header the backend compiles against** — so if
Satellite's use of an ABI is wrong (a misnamed field, a bad struct assumption) it fails here
rather than on a user's machine.

Each is built under the name its loader searches for, and the **real** loader is what finds
it. Nothing about either load path is mocked:

- the NDI fake is built as `libndi.so.6` (`libndi.dylib` on macOS) with
  `NDI_RUNTIME_DIR_V6` pointed at its directory
- the OMT fake is built as `libomt.so` next to the test binaries, which exercises the
  backend's real "bundled beside the plugin" search path, with no environment variable at all

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

### `omt-test` — both directions over OMT

- The library is found through the bundled-beside-the-plugin path, every flat C symbol binds,
  and the configured discovery server and port range are pushed into libomt at load
- Discovery returns sources, and the strings are copied out — verified by calling discovery
  again, since libomt's array is only valid until its next call
- A receiver is created with the right frame types, preferred format and suggested quality
- UYVY, BGRA and BGRX are all decoded, including the detail that **OMT signals BGRA vs BGRX
  through an alpha flag rather than a different codec**
- Audio arrives as planar float with the planes packed back to back
- Tally is sent upstream, and statistics come back with `bitrate_estimated` **false** — OMT
  reports real byte counters, so unlike NDI the figure is exact
- A sender maps formats and timestamps correctly, leaves the alpha flag clear for opaque
  frames (so OMT keeps BGRX semantics), and reports tally and connections
- `omt_shutdown` runs before the library handle is released, so libomt's own background
  threads do not outlive the code they are in

## What it does not cover

These are fakes, so they prove Satellite drives each ABI correctly — not that the real
libraries behave the way the fakes do. Timing, reconnection, genuine network behaviour and
interop with real senders still need machines with the actual runtimes installed.

The OMT side has been checked against the **real** library once, by hand: `build-aux/build-omt`
produces a working `libomt`, Satellite's loader binds all sixteen entry points against it
(the real library version-tags its exports as `omt_…@@V1.0`, and `dlsym` on the bare names
resolves them), and discovery starts. A full send-to-receive loopback could not be completed
in the sandbox, which has no IPv6 — libomt's socket setup fails with
`SocketException (97): Address family not supported by protocol`. That is environmental, but
it means the real round trip is still unverified.
