# Satellite

An OBS Studio plugin that carries video, audio, metadata and tally between OBS and other
machines on a local network, over **either NDI or OMT ([Open Media
Transport](https://github.com/openmediatransport))**, behind one shared set of OBS
source/filter/output types and one management window.

> **Status: early development.** The architecture, build and UI scaffolding are in place and
> the plugin loads, but neither protocol backend is implemented yet — nothing sends or
> receives video today. See [the roadmap](docs/ARCHITECTURE.md#13-roadmap).

## Why

Running NDI and OMT today means running two plugins that each grab the same frames. Satellite
puts both transports behind one abstraction so you can pick the protocol per feed — or switch
an existing feed from one to the other — without rebuilding a scene or learning a second
plugin.

## Planned features

**Shared across both protocols**

- **Satellite Source** — receive a network feed into OBS, with a protocol dropdown
- **Satellite Sender** — a filter that publishes any source on the network, video and audio
- **Program and Preview outputs** — independently configurable, Preview follows Studio Mode
- **Full bidirectional tally** — publish OBS's program/preview state upstream, and surface
  the tally your sources report
- **The Satellite window** — an OBS dock listing every active feed in either direction with
  its state, format, bitrate, dropped frames and a rolling history sparkline
- **Runtime status** — whether each protocol's library is present, with an install link for
  NDI
- **DistroAV import** — a one-time, non-destructive offer to convert an existing setup

## Installing the runtimes

**OMT** ships with Satellite. Nothing to install.

**NDI** does not, and cannot: the NDI SDK is proprietary and not redistributable. Satellite
loads it at run time and the Satellite window tells you if it is missing, with a link to
[ndi.video/tools](https://ndi.video/tools/). This is the same approach DistroAV takes, for
the same reason.

## Building

Satellite uses the standard [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
build, with Qt and the frontend API enabled. Neither transport runtime is needed to build —
both are resolved at run time.

```sh
cmake --preset ubuntu-x86_64     # or windows-x64, macos
cmake --build --preset ubuntu-x86_64
```

## Documentation

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is the design specification: the decisions
taken and why, how the two protocol APIs map onto one abstraction, the threading model, and
the roadmap.

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).

- **OMT** (`libomt`, `libomtnet`, `libvmx`) is MIT-licensed and is bundled with Satellite.
- **NDI** is proprietary. Satellite links to it only at run time and ships none of it. NDI®
  is a registered trademark of Vizrt NDI AB; Satellite is not affiliated with or endorsed by
  Vizrt.
- **DistroAV** is GPL-2.0. Any code derived from it is marked as such in the file that does so.
