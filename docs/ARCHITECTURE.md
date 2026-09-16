# Satellite — Architecture &amp; Design Specification

> Status: **draft v0.6** — feature complete against the original brief (M1–M4). NDI and OMT
> both work in both directions, the OMT libraries build from source, and the Satellite window
> carries the metrics, advanced settings and DistroAV import. This document is the contract the
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
| 3 | OMT native libraries | Build `libomt` and `libvmx` from pinned sources in CI and ship them | Upstream publishes no binaries at all, so the original "bundle prebuilt" answer was not available — see §11.1 |
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
only — nothing at run time needs .NET installed. See §11.1 for why this matters more than it
first appeared to.

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
| NDI exposes no byte counters | `FeedStats::bitrate_mbps` is measured by Satellite from frame sizes for NDI (and flagged `bitrate_estimated`), and read exactly from `OMTStatistics` for OMT. The dock marks the estimated ones with a `~`. |
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
| Send | one per active sender (filter or output) | Drains a bounded queue into `send_video`/`send_audio`, and polls tally and counters |
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

Bidirectional, per decision #6. Note which side does which — it is the opposite of what the
names suggest, because in both protocols it is the *receiver* that reports tally upstream:

- **Outbound, from our sources.** A Satellite Source tells the remote sender whether this
  OBS has it on program or preview, via `NDIlib_recv_set_tally` / `omt_receive_settally`.
  That is how a remote camera learns it is live here. It is driven by OBS's own `activate`
  /`deactivate` (program) and `show`/`hide` (visible anywhere, which includes the Studio
  Mode preview) source callbacks, so it is event-driven and nothing walks the scene graph on
  a timer. A source on program is also "showing", so preview is reported only when showing
  and *not* active.
- **Inbound, at our senders.** A Satellite sender polls `send_get_tally` /
  `omt_send_gettally` from its send thread to learn whether a downstream receiver has *us*
  on program, and surfaces it in the dock.

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
| Conn | connections, for senders |
| History | sparkline of bitrate over the last ~60 samples |

Frame rate, codec time, peak bitrate and whether the bitrate is exact or measured go in each
row's tooltip rather than in columns. A dock is narrow, and a column each for numbers people
look at once would make the table unreadable.

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

Per decision #7, on first run — once the scene collection has loaded, which is why it hangs
off `OBS_FRONTEND_EVENT_FINISHED_LOADING` rather than module load — Satellite scans for
DistroAV items and, if it finds any, offers a dialog listing exactly what would be converted.
Every row is individually checkable. It is also available any time from the Satellite window,
so declining the offer is not a one-way door.

What gets scanned, with the identifiers read out of DistroAV's own source rather than
guessed:

| DistroAV | Becomes |
|---|---|
| `ndi_source` input | Satellite Source on `Protocol::NDI`, carrying `ndi_source_name`, `ndi_audio`, and `ndi_bw_mode` mapped onto `Quality` |
| `ndi_filter` | Satellite Sender, carrying `ndi_filter_ndiname` |
| `ndi_audiofilter` | Satellite Sender, **flagged**: Satellite has one sender filter and it carries video too, so this is not an exact conversion |
| `MainOutputName` / `PreviewOutputName` in OBS's config under `NDIPlugin` | The Program and Preview sender settings |

`ndi_bw_mode` is DistroAV's `PROP_BW_*`: 0 highest, 1 lowest, 2 audio-only, -1 undefined.
Anything unrecognised maps to full quality, because guessing lower would silently degrade a
feed with no indication why. `tests/import-test` pins all of that, since those numbers are
another project's internals and nothing would otherwise notice them changing.

**Non-destructive by default.** Converted sources are added alongside the DistroAV ones,
mirrored into every scene that used them with the same transform and visibility, and the
originals are removed only if the user ticks the box. Imported output settings are written
but **not enabled** — converting settings is one thing, putting a feed on the network
unasked is another.

## 10.1 Live settings

Advanced settings are edited in the Satellite window and applied without restarting OBS, but
the two protocols can absorb a change at different moments, so `IBackend::settings_changed()`
lets each decide:

- **OMT** applies immediately. `omt_settings_*` only affect instances created afterwards and
  libomt guards them internally.
- **NDI** cannot. Groups are baked into the finder at creation, so a change means a new
  finder — and the discovery thread is using the old one. The backend therefore only marks it
  stale, and the discovery thread rebuilds it at the top of its next cycle, where it is the
  only thread involved.

The same reasoning governs the runtime re-check button: it retries `load()` only for
backends that are **not** available. A loaded backend is in use by the discovery thread and
by every live source, so reloading it underneath them would be a crash rather than a refresh.
Restricted that way, installing the NDI runtime becomes a click instead of an OBS restart.

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

### 11.1 The OMT libraries are not actually distributed

Decision #3 was "bundle upstream prebuilt binaries". That rested on a premise that turned out
to be false, and the correction is recorded here because it changes what is possible.

**Upstream publishes no binaries.** Neither `libomt` nor `libvmx` has a single GitHub
release, and neither commits build output. Both are source-only:

| Repository | Language | Build requirement |
|---|---|---|
| `libomt` | C# | **.NET 8 SDK**, NativeAOT, per-platform scripts in `build/` |
| `libvmx` | C / C++ | A C++ compiler, per-platform scripts in `build/` |

Prebuilt copies do exist inside the release packages of `omtplugin`, the reference OBS
plugin — which is where the impression that upstream ships libraries comes from. Those are
that project's packaging, not a library distribution.

**Satellite therefore builds both from source.** `build-aux/build-omt` (and
`build-aux/Build-Omt.ps1` on Windows) clones all three repositories at commits pinned in
`buildspec.json`, builds `libomtnet`, then `libomt` with NativeAOT, then `libvmx`, and leaves
the two shared libraries in `deps/omt/`. CI runs it before configuring the plugin, and CMake
installs whatever it finds beside the plugin binary — which is the first place the OMT
backend looks. If the directory is empty the plugin still builds and runs, and OMT simply
reports itself unavailable, exactly as NDI does without its runtime.

Two things that are easy to trip over, both found by actually running the build:

- **`libomt` references `libomtnet` by a relative `HintPath`**, so the two have to be cloned
  as siblings and `libomtnet` built first. Cloning `libomt` alone does not build.
- **`libvmx` does not compile with current clang.** It initialises byte arrays from negative
  literals; older clang warned, clang 18+ rejects it. Upstream's own build script has no flag
  for this, so Satellite's adds `-Wno-c++11-narrowing`.

### 11.2 libomt aborts the process without an Avahi daemon

On Linux libomt does DNS-SD discovery through `avahi-client`, and **avahi-client does not
report a missing daemon as an error — it fails an assertion and calls `abort()`**:

```
avahi_service_browser_new: Assertion `client' failed.
```

Inside OBS that kills the whole application. It is not catchable, and libomt starts discovery
lazily on the first sender or receiver, so the abort would land in the middle of a show
rather than at load time.

So the OMT backend checks for the daemon's client socket before declaring itself available,
and declines to load with an explanatory message if it is absent. The check is skipped when a
discovery server is configured, because that path uses TCP instead of Avahi and genuinely
does not need the daemon.

This is the one place Satellite refuses to use a library that is present and loadable. The
alternative is a crash with no diagnostic in someone's production stream.

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
| **M2 — NDI send** *(landed)* | NDI sender, bounded send queue with a drop policy, Program and Preview outputs, Sender filter over a dedicated view, bidirectional tally, output controls in the dock |
| **M3 — OMT parity** *(landed)* | Vendored `libomt.h`, runtime loader binding the flat C exports, OMT receiver and sender with full frame conversion, discovery, tally and exact statistics, fake-libomt test harness, the libraries built from pinned sources and packaged, and the Avahi guard (§11.2) |
| **M4 — polish** *(landed)* | DistroAV import behind a confirmation dialog, full metrics in the dock with per-row detail, live advanced settings for both protocols, and a runtime re-check that avoids an OBS restart after installing NDI |
| **M5 — release** | Three-platform CI packaging, codesigning/notarization, docs |

## 14. Open questions

Not blocking M0/M1; worth settling before the milestone that needs them.

1. ~~**NDI SDK version floor**~~ — settled: **v6 only**. `NDIlib_v6_load` is the single
   entry point bound, and the runtime search uses `NDILIB_REDIST_FOLDER` /
   `NDILIB_LIBRARY_NAME` from the vendored headers so it tracks the SDK version rather than
   hard-coding paths.
2. ~~**Sender naming**~~ — settled, and it turned out not to be a choice: NDI itself
   presents a sender as `MACHINE (name)`. The machine prefix is the runtime's doing, so
   Satellite passes the bare name and gets DistroAV's convention for free.
3. **Per-feed CPU** — still open, and less pressing than it was: OMT's `CodecTime` is
   surfaced in the row tooltip, which is most of the value for the case that matters. Real
   per-thread CPU remains platform-specific work nobody has asked for yet.
4. **Failover** — should a source be able to hold both an NDI and an OMT address and fall
   back? Attractive, and outside v1 scope. (post-v1)
