# Satellite

An OBS Studio plugin that carries video, audio, metadata and tally between OBS and other
machines on a local network, over **either NDI or OMT ([Open Media
Transport](https://github.com/openmediatransport))**, behind one shared set of OBS
source/filter/output types and one management window.

> **Status: early development.** **Both protocols now work in both directions** — receive a
> feed into OBS, or publish your Program, Preview, or any single source onto the network,
> with tally, over NDI or OMT.
>
> One caveat for OMT: upstream publishes no prebuilt libraries and Satellite does not yet
> build them, so OMT shows as unavailable until you supply `libomt` yourself. See
> [`lib/omt`](lib/omt/README.md) and [the roadmap](docs/ARCHITECTURE.md#13-roadmap).

## Why

Running NDI and OMT today means running two plugins that each grab the same frames. Satellite
puts both transports behind one abstraction so you can pick the protocol per feed — or switch
an existing feed from one to the other — without rebuilding a scene or learning a second
plugin.

## Features

**Working today** — over NDI, and over OMT once its libraries are present

- **Satellite Source** — discovers feeds on the network and receives video and audio
- **Satellite Sender** — a filter that publishes any source on the network. It renders the
  source through a view of its own rather than intercepting async frames, so it works on
  game capture, browser sources and scenes, not just cameras and media files
- **Program and Preview outputs** — independently configurable from the Satellite window;
  Preview runs only while Studio Mode is active
- **Full bidirectional tally** — your sources tell remote senders when they are live here,
  and your senders show whether someone downstream has you on air
- **The Satellite window** — an OBS dock listing every active feed in either direction with
  its state, format, bitrate, dropped frames and a rolling history sparkline
- **Runtime status** — whether each protocol's library is present, with an install link for
  NDI

**Planned**

- **Shipping the OMT libraries** — see [`lib/omt`](lib/omt/README.md)
- **DistroAV import** — a one-time, non-destructive offer to convert an existing setup

## Installing the runtimes

Neither library is linked at build time. Satellite loads both at run time, and the Satellite
window shows which of them it found.

**NDI** cannot be shipped: the SDK is proprietary and not redistributable. Install the NDI 6
runtime from [ndi.video/tools](https://ndi.video/tools/) — the Satellite window links to it
when it is missing. This is the same approach DistroAV takes, for the same reason.

**OMT** is MIT-licensed and could be shipped, but upstream publishes no binaries for it and
Satellite does not build them yet, so for now you need to supply `libomt` (and `libvmx`)
yourself — built from source, or taken from an
[`omtplugin`](https://github.com/openmediatransport/omtplugin) install. Satellite looks
beside its own binary first, then on the system library path. See
[`lib/omt`](lib/omt/README.md).

## Building

Satellite uses the standard [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
build, with Qt and the frontend API enabled. Neither transport runtime is needed to build —
both are resolved at run time.

```sh
cmake --preset ubuntu-x86_64     # or windows-x64, macos
cmake --build --preset ubuntu-x86_64
```

Interface headers for both protocols are vendored: [`lib/ndi`](lib/ndi/README.md) (each file
carries its own MIT licence, separate from the SDK) and [`lib/omt`](lib/omt/README.md) (MIT).
Neither SDK needs installing to build.

### Tests

```sh
cmake -S . -B build -DENABLE_TESTS=ON && cmake --build build
ctest --test-dir build --output-on-failure
```

Neither runtime can be installed in CI, so the test suite builds fakes for both against the
same vendored headers and drives the real loaders against them — including a slow-sink mode
that exercises what the send queue does under backpressure. See [`tests/`](tests/README.md).

## Documentation

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is the design specification: the decisions
taken and why, how the two protocol APIs map onto one abstraction, the threading model, and
the roadmap.

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).

- **OMT** (`libomt`, `libomtnet`, `libvmx`) is MIT-licensed. Its header is vendored here;
  the libraries are not yet shipped.
- **NDI** is proprietary. Satellite links to it only at run time and ships none of it. NDI®
  is a registered trademark of Vizrt NDI AB; Satellite is not affiliated with or endorsed by
  Vizrt.
- **DistroAV** is GPL-2.0. Any code derived from it is marked as such in the file that does so.
