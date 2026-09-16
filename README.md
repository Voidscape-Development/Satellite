# Satellite

An OBS Studio plugin that carries video, audio, metadata and tally between OBS and other
machines on a local network, over **either NDI or OMT ([Open Media
Transport](https://github.com/openmediatransport))**, behind one shared set of OBS
source/filter/output types and one management window.

> **Status: feature complete, not yet battle-tested.** Both protocols work in both
> directions — receive a feed into OBS, or publish your Program, Preview, or any single
> source onto the network, with tally, over NDI or OMT.
>
> It has not yet been run inside a real OBS against real hardware. If you try it, please
> report what breaks.

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
  NDI and a re-check button so installing it does not need an OBS restart
- **DistroAV import** — converts an existing DistroAV setup, showing exactly what it will do
  first and leaving your originals alone unless you ask otherwise
- **Advanced settings** — NDI groups, and the OMT discovery server and port range, applied
  without restarting OBS

## Installing the runtimes

Neither library is linked at build time. Satellite loads both at run time, and the Satellite
window shows which of them it found.

**NDI** cannot be shipped: the SDK is proprietary and not redistributable. Install the NDI 6
runtime from [ndi.video/tools](https://ndi.video/tools/) — the Satellite window links to it
when it is missing. This is the same approach DistroAV takes, for the same reason.

**OMT** ships with Satellite. Upstream publishes no binaries, so Satellite builds `libomt`
and `libvmx` from pinned sources and packages them beside the plugin — see
[`lib/omt`](lib/omt/README.md).

On **Linux**, OMT also needs the **Avahi daemon** running. libomt discovers sources through
`avahi-client`, which aborts the process rather than returning an error if the daemon is
missing, so Satellite declines to enable OMT without it instead of risking taking OBS down.
Install and start `avahi-daemon`, or configure an OMT discovery server.

## Building

Satellite uses the standard [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
build, with Qt and the frontend API enabled. Neither transport runtime is needed to build —
both are resolved at run time.

```sh
build-aux/build-omt              # builds libomt + libvmx into deps/omt
cmake --preset ubuntu-x86_64     # or windows-x64, macos
cmake --build --preset ubuntu-x86_64
```

The first step needs the **.NET 8 SDK** and **clang** — `libomt` is C# compiled to a native
library with NativeAOT. It is a build-time requirement only; nothing at run time needs .NET.
Skip it and the plugin still builds, with OMT reporting itself unavailable. On Windows run
`pwsh build-aux/Build-Omt.ps1` instead.

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

- **OMT** (`libomt`, `libomtnet`, `libvmx`) is MIT-licensed. Its header is vendored here and
  the libraries are built from pinned sources and shipped with the plugin.
- **NDI** is proprietary. Satellite links to it only at run time and ships none of it. NDI®
  is a registered trademark of Vizrt NDI AB; Satellite is not affiliated with or endorsed by
  Vizrt.
- **DistroAV** is GPL-2.0. Any code derived from it is marked as such in the file that does so.
