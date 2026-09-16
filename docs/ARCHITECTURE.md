# Satellite — Architecture &amp; Design Specification

> Status: **draft v0.2** — design agreed, scaffolding landed, NDI receive implemented (M1).
> OMT and the send paths are not yet implemented. This document is the contract the
> implementation is built against; change it in the same PR that changes the behaviour it
> describes.

## 1. What Satellite is

Satellite is an OBS Studio plugin that carries video, audio, metadata and tally between
OBS and other machines on a local network, over **either NDI or OMT (Open Media
Transport)**, behind one shared set of OBS source/filter/output types and one management
window.

The premise is that a producer should be able to pick the transport per feed — or switch
an existing feed from one to the other — without rebuilding a scene, learning a second
plugin, or running two plugins that each grab the same frames.

### Goals

- One unified Source, one unified Sender filter, one Program/Preview output path, each
  with a protocol selector.
- Feature parity with DistroAV for NDI, plus the same feature set for OMT.
- A Satellite window (OBS dock) showing every active feed in either direction, its health,
  and its throughput.
- Full bidirectional tally.
- Lower overhead than running two separate plugins: one discovery thread total, one
  frame-conversion path, shared buffers.

### Non-goals (for v1)

- WAN / internet transport, NDI Bridge equivalents, cloud relays.
- Recording or file muxing of received compressed frames (OMT can hand us VMX1 —
  noted as a future hook, not built).
- NDI KVM, PTZ control surfaces.

## 2. Decisions taken

These were settled before implementation began and are recorded so the reasoning does not
have to be rediscovered.

| # | Decision | Choice | Why |
|---|---|---|---|
| 1 | First deliverable | Spec doc + compiling scaffolding | Reviewable shape before protocol code exists |
| 2 | OBS UI shape | Unified types with a protocol dropdown | The point of the plugin is switching transport without rebuilding scenes |
| 3 | OMT native libraries | Bundle upstream prebuilt binaries | MIT-licensed and redistributable; avoids .NET 8 in CI |
| 4 | Platforms | Windows, Linux, macOS | All three, Windows/Linux first through CI |
| 5 | Metrics | Network + health with sparklines | Exact values both SDKs already expose; no charting dependency |
| 6 | Tally | Full bidirectional | The main reason NDI gets used in multi-machine production |
| 7 | DistroAV migration | One-time import behind a confirmation dialog | Non-destructive; leaves DistroAV items alone unless approved |
| 8 | Satellite window | Native OBS dock + Tools menu entry | Remembers position, docks/tabs/floats like Audio Mixer |

### 2.1 Two decisions that deviate from the original sketch

**Discovery is not duty-cycled.** The original spec asked for "scan 3–5s, then update every
5–10s". That is not implemented, because it is worse on both axes it was meant to improve:

- The expensive part of NDI discovery is *creating and destroying* the finder, which a
  duty-cycled loop does repeatedly. A long-lived finder blocked in
  `NDIlib_find_wait_for_sources()` is nearly free — it is a socket wait.
- Tearing the finder down means sources that appear during the sleep are invisible for up
  to 10 seconds, and sources that vanish linger just as long.

Instead: **one long-lived finder per protocol, on one shared background thread**, with the
*UI refresh* throttled instead of the discovery itself. See §6.

**NDI cannot be bundled; OMT can.** The NDI SDK and runtime are proprietary and not
redistributable. Satellite therefore loads the NDI runtime dynamically at run time and, if
it is missing, tells the user how to install it — the same approach DistroAV takes, for the
same legal reason. OMT is MIT, so its libraries ship inside the Satellite package. This
asymmetry is deliberate and is modelled explicitly in `IBackend` (§5) rather than papered
over.

## 3. Protocol landscape

Findings from reading the upstream sources, recorded because they drive the whole design.

### 3.1 OMT

Upstream (github.com/openmediatransport) ships:

| Component | Language | Role |
|---|---|---|
| `libomtnet` | C# / .NET Standard 2.0 | The protocol implementation |
| `libvmx` | C | The VMX video codec |
| `libomt` | C# built with .NET **NativeAOT** | Thin shim exporting a C API (`libomt.h`) |

`libomtnet` is statically linked into `libomt` by the NativeAOT build, so at run time
Satellite needs only **`libomt` + `libvmx`**. The .NET 8 SDK is a *build-time* requirement
only, which is why decision #3 (bundle upstream prebuilt binaries) avoids it entirely.

Discovery is DNS-SD/mDNS or multicast UDP, with an optional TCP discovery server for
networks where multicast is blocked (`omt_settings_set_string("DiscoveryServer", "omt://host:port")`).

### 3.2 The two APIs line up better than expected

| Concern | NDI | OMT |
|---|---|---|
| Discovery | `NDIlib_find_wait_for_sources(timeout)` — **blocking, event-driven** | `omt_discovery_getaddresses(&count)` — **polled snapshot**, background thread inside libomt |
| Receiver | `NDIlib_recv_create_v3` | `omt_receive_create(address, frameTypes, format, flags)` |
| Capture | `NDIlib_recv_capture_v3(timeout)` | `omt_receive(inst, frameTypes, timeout_ms)` |
| Frame lifetime | explicit `NDIlib_recv_free_video_v2` | implicit — valid until the next `omt_receive` for that instance **and frame type** |
| Sender | `NDIlib_send_create` | `omt_send_create(name, quality)` |
| Send | `NDIlib_send_send_video_v2` / `_audio_v3` | `omt_send(inst, &frame)` |
| Tally out (receiver → sender) | `NDIlib_recv_set_tally` | `omt_receive_settally` |
| Tally in (at the sender) | `NDIlib_send_get_tally(timeout)` | `omt_send_gettally(inst, timeout_ms, &tally)` |
| Connection count | `NDIlib_send_get_no_connections` | `omt_send_connections` |
| Statistics | `NDIlib_recv_get_performance` — frame counts only | `OMTStatistics` — bytes, frames, dropped, codec time |

Crucially, the **pixel and sample formats match OBS natively on both sides**:

- Both protocols prefer **UYVY** for video, which is `VIDEO_FORMAT_UYVY` in OBS. The common
  path needs no conversion.
- Both carry **32-bit planar float audio** (OMT calls it `FPA1`, NDI's audio v3 is the
  same layout), which is OBS's internal `AUDIO_FORMAT_FLOAT_PLANAR`. Again no conversion.
- Both express timestamps in **100 ns units**. OBS uses nanoseconds, so the conversion is a
  single `* 100`.

This is what makes a single abstraction honest rather than a lowest-common-denominator
compromise.

### 3.3 Where they differ, and how the abstraction handles it

| Difference | Handling |
|---|---|
| NDI discovery blocks; OMT discovery polls | `IBackend::wait_for_sources(timeout_ms)` — NDI blocks in the SDK, OMT sleeps. One thread, one cadence (§6). |
| `omt_discovery_getaddresses` returns an array valid only until the next call | Exactly one thread may call it, and it copies under lock before returning. This is the single-discovery-thread design, enforced by the API. |
| NDI exposes no byte counters | `FeedStats::bitrate_mbps` is measured by Satellite from frame sizes for NDI, and read from `OMTStatistics::BytesSentSinceLast` for OMT. The dock marks estimated values. |
| Quality knobs are shaped differently — OMT has `OMTQuality` Default/Low/Medium/High; NDI has bandwidth modes highest/lowest/audio-only | A common `Quality` enum maps onto each. `OMTReceiveFlags_Preview` (1/8 preview) and `NDIlib_recv_bandwidth_lowest` both back the shared "low bandwidth preview" mode. |
| Discovery server configuration | Common Advanced setting, written through `omt_settings_set_string` / the NDI equivalent. |
| HDR / high bit depth | OMT `P216`/`PA16`, NDI `P216`. Common `HighBitDepth` flag; deferred past v1 but the frame struct carries it. |
| Alpha | OMT needs `OMTVideoFlags_Alpha` with `BGRA`/`UYVA`; NDI uses `BGRA`. Common `has_alpha` flag. |

## 4. Module layout

```
src/
  plugin-main.cpp              module load/unload, registration order
  plugin-support.{h,c.in}      template logging shim (obs_log)

  transport/
    transport.hpp              protocol enum, frame/stats/tally structs, IBackend/ISender/IReceiver
    transport.cpp              backend registry, protocol <-> string
    library-loader.{hpp,cpp}   cross-platform dlopen/LoadLibrary + symbol resolution
    ndi/ndi-backend.{hpp,cpp}  NDI runtime discovery, load, send/recv
    omt/omt-backend.{hpp,cpp}  libomt load, send/recv

  discovery/
    discovery-service.{hpp,cpp}  single background thread, merged source registry

  metrics/
    feed-registry.{hpp,cpp}    live feed table + rolling history for sparklines

  obs/
    satellite-source.{hpp,cpp} obs_source_info — unified receiver
    satellite-filter.{hpp,cpp} obs_source_info — unified sender filter
    satellite-output.{hpp,cpp} obs_output_info + Program/Preview wiring

  ui/
    satellite-dock.{hpp,cpp}   the Satellite window
    sparkline.{hpp,cpp}        tiny self-painted history widget, no chart dependency

  config/
    config.{hpp,cpp}           global plugin settings, load/save
```

Dependency direction is strictly downward: `ui` and `obs` depend on `discovery`, `metrics`
and `transport`; `transport` depends on nothing inside the plugin except `plugin-support`.
Nothing below `obs/` calls `obs_frontend_*`.

## 5. The transport abstraction

Three interfaces, in `src/transport/transport.hpp`.

**`IBackend`** — one per protocol, created at module load, owns the runtime library handle.
Because a runtime may be absent, it models availability as first-class state:

```
bool         available()
std::string  unavailable_reason()   // shown in the dock
std::string  runtime_version()
std::string  install_url()          // NDI: install page. OMT: empty, we bundle it.
bool         load() / void unload()
```

It also owns discovery (`wait_for_sources`, `poll_sources`) and is the factory for
`IReceiver` / `ISender`.

**`IReceiver`** — `capture(timeout_ms, CapturedFrame&)`, `set_tally()`, `stats()`.

**`ISender`** — `send_video()`, `send_audio()`, `tally()`, `connections()`, `stats()`.

A normalized `VideoFrame` / `AudioFrame` pair sits between the backends and OBS. They are
deliberately shaped like `obs_source_frame` / `obs_source_audio` so the common UYVY and
planar-float paths are a field copy, not a pixel conversion.

### Frame lifetime rule

OMT's contract is that captured frame data is valid only until the next `omt_receive` call
for the same instance and frame type; NDI requires an explicit free. Satellite therefore
holds this invariant: **a `CapturedFrame` is borrowed, not owned.** It must be handed to
`obs_source_output_video`/`_audio` (both of which copy internally) and released before the
next capture on that receiver. `CapturedFrame`'s destructor performs the backend-specific
release, so the rule is enforced by scope rather than by discipline.

## 6. Threading model

| Thread | Owner | Work |
|---|---|---|
| Discovery | `DiscoveryService`, one process-wide | Round-robins backends: `wait_for_sources(~1000 ms)` then `poll_sources()`, merges into the registry, signals changes |
| Receive | one per active Satellite Source | `capture()` loop, pushes into OBS |
| Send | one per active sender (filter or output) | Drains a bounded queue into `send_video`/`send_audio` |
| Tally poll | folded into the send thread | `send_gettally` with a short timeout |
| UI | Qt main thread | Reads registry snapshots on a timer; never blocks on a backend call |

The discovery thread is the only caller of `omt_discovery_getaddresses`, as the API's
buffer-lifetime contract requires. Source lists reach the UI as copied snapshots under a
mutex; the UI never holds a backend lock.

Feed metrics are written by the receive/send threads into per-feed atomics and sampled by
the UI timer, so the dock never adds latency to a frame path.

## 7. OBS integration

### 7.1 Satellite Source (receive)

One `obs_source_info`, async video + audio. Properties:

- **Protocol** — NDI / OMT
- **Source** — combo populated from the discovery registry, filtered to the chosen
  protocol, plus free-text entry for a direct address (`omt://host:port`, or an NDI
  machine name) when mDNS is blocked
- **Bandwidth / Quality** — Full / Preview (low bandwidth) / Audio only
- **Audio** — enable, channel layout
- **Sync** — OBS buffering vs. source timestamps
- **Advanced** — latency mode, alpha, hardware acceleration hints

Switching Protocol rebuilds the receiver in place; the scene item survives.

### 7.2 Satellite Sender (filter)

One `obs_source_info` of type filter, attachable to any source. Properties: protocol,
sender name, quality, and **include audio** (default on, with a toggle — the filtered
source's audio is captured alongside its video rather than being video-only).

### 7.3 Program / Preview output

An `obs_output_info` plus frontend wiring, configured from the Satellite window rather than
from a source: independent enable, name and quality for Program and for Preview, and the
Preview sender starts only when Studio Mode is active.

### 7.4 Tally

Bidirectional, per decision #6:

- **Outbound** — Satellite senders publish OBS program/preview state upstream, driven by
  `OBS_FRONTEND_EVENT_SCENE_CHANGED` / `_PREVIEW_SCENE_CHANGED` and studio-mode events.
- **Inbound** — Satellite sources report the tally the remote sender sees, surfaced in the
  dock, and pushed back out with `NDIlib_recv_set_tally` / `omt_receive_settally` so a
  remote camera knows it is live in this OBS.

## 8. The Satellite window

A native OBS dock (`obs_frontend_add_dock_by_id`) plus a **Tools → Satellite** item that
shows and raises it.

**Runtime status strip** — one row per protocol: detected or not, version, and for NDI an
install link when the runtime is missing. This is the DistroAV-style "is the NDI library
installed" affordance.

**Feed table** — one row per active feed, either direction:

| Column | Source |
|---|---|
| Name | sender name / source address |
| Protocol | NDI or OMT |
| Direction | Send / Receive |
| State | Connected, connecting, idle, error |
| Format | resolution, fps, pixel format |
| Audio | channels, sample rate |
| Bitrate | OMT: `BytesSentSinceLast`. NDI: measured by us, marked estimated |
| Dropped | OMT: `FramesDropped`. NDI: `NDIlib_recv_get_performance` dropped counters |
| Connections | senders only |
| Tally | program / preview indicators |
| History | sparkline of bitrate over the last ~60 samples |

**Output controls** — Program and Preview sender configuration, per §7.3.

`Sparkline` is a ~100-line self-painted `QWidget` over a ring buffer. No charting library,
no new dependency, and it respects the OBS theme palette.

## 9. Configuration

Global plugin settings (not per-profile) in the module config directory, via
`obs_data_t`. Covers Program/Preview sender config, discovery preferences, per-protocol
advanced settings (NDI groups, OMT discovery server and port range), and the "DistroAV
import already offered" flag.

Per-source and per-filter settings live in the normal OBS settings blob, so they travel
with the scene collection.

## 10. DistroAV migration

Per decision #7, on first run Satellite scans the loaded scene collection for DistroAV
sources and filters and its saved configuration, and if it finds any, offers a dialog
listing exactly what would be converted. Nothing is touched without confirmation, and
DistroAV's own items are left in place unless the user approves replacing them.

The mapping is mechanical — DistroAV is NDI-only, so every import lands on
`Protocol::NDI` with the same source name, bandwidth and audio settings.

## 11. Build and packaging

Based on the standard `obs-plugintemplate` CMake, with `ENABLE_QT` and
`ENABLE_FRONTEND_API` turned **ON** and the module built as C++ (C++17).

Neither runtime is linked at build time. Both are resolved through
`transport/library-loader` at module load, so a missing runtime degrades to "protocol
unavailable" in the dock instead of a module that refuses to load.

| Platform | NDI | OMT |
|---|---|---|
| Windows | `LoadLibrary` on the redist named by `NDI_RUNTIME_DIR_V6`/`V5` | `libomt.dll` + `libvmx.dll` bundled next to the plugin |
| macOS | `dlopen` the installed NDI framework | `.dylib`s bundled in the plugin bundle, universal arm64+x86_64, codesigned and notarized |
| Linux | `dlopen` `libndi.so.*` from the installed runtime | `.so`s bundled in the plugin data dir, loaded by absolute path |

Vendored OMT binaries live under `deps/omt/<platform>/` with their upstream version and
SHA-256 recorded in `buildspec.json`, so an update is a reviewable diff rather than a
silent drop-in.

## 12. Licensing

Satellite is **GPL-2.0-or-later**, matching the `obs-plugintemplate` default and DistroAV.

- **DistroAV** is GPL-2.0, so its code may be reused here with attribution. Any file that
  does so must say which DistroAV file it derives from.
- **OMT** (`libomt`, `libomtnet`, `libvmx`) is MIT — compatible, redistributable, and
  bundled. Upstream copyright and the MIT text ship in the package.
- **NDI** is proprietary. Satellite links to it only at run time through `dlopen`, ships
  none of the runtime, and requires the user to install it themselves. NDI® is a registered
  trademark of Vizrt NDI AB; Satellite is not affiliated with or endorsed by Vizrt.

  The SDK's *interface headers* are a separate matter and are vendored in `lib/ndi`: each
  one carries its own MIT licence notice ("the following MIT license applies to this file
  ONLY and not to the SDK as a whole"), so redistributing the headers is explicitly
  permitted. That is what makes building without an installed SDK possible, and it is why
  this is not the licence compromise it first appears to be. See `lib/ndi/README.md`.

## 13. Roadmap

| Milestone | Contents |
|---|---|
| **M0 — scaffolding** *(landed)* | Spec, renamed template, Qt + frontend API enabled, transport abstraction, stub backends, discovery service, feed registry, dock skeleton, source/filter/output registered. Compiles and loads; no protocol traffic. |
| **M1 — NDI receive** *(landed)* | Vendored NDI 6 headers, runtime loader, real discovery, receiver with full frame conversion, Satellite Source pushing video and audio into OBS, dock showing live receive feeds, fake-runtime test harness |
| **M2 — NDI send** | Sender filter, Program/Preview outputs, bidirectional tally |
| **M3 — OMT parity** | `libomt` loader, vendored binaries, OMT source/filter/output, unified discovery |
| **M4 — polish** | DistroAV import, sparklines and full metrics, advanced per-protocol settings, install-helper flows |
| **M5 — release** | Three-platform CI packaging, codesigning/notarization, docs |

## 14. Open questions

Not blocking M0/M1; worth settling before the milestone that needs them.

1. ~~**NDI SDK version floor**~~ — settled: **v6 only**. `NDIlib_v6_load` is the single
   entry point bound, and the runtime search uses `NDILIB_REDIST_FOLDER` /
   `NDILIB_LIBRARY_NAME` from the vendored headers so it tracks the SDK version rather than
   hard-coding paths.
2. **Sender naming** — DistroAV uses `MACHINE (source name)`. Keep that convention for
   familiarity, or make it a template string? (M2)
3. **Per-feed CPU** — deliberately excluded from decision #5. OMT hands us `CodecTime` for
   free, which is most of the value. Add real per-thread CPU later, or leave it? (M4)
4. **Failover** — should a source be able to hold both an NDI and an OMT address and fall
   back? Attractive, and outside v1 scope. (post-v1)
