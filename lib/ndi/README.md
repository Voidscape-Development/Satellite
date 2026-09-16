# Vendored NDI SDK headers

These are the interface headers from the NDI SDK, copyright © Vizrt NDI AB, vendored here so
that Satellite builds without every developer and CI runner having to install the SDK first.

**Each of these files carries its own MIT licence**, stated at the top of the file:

> NOTE: The following MIT license applies to this file ONLY and not to the SDK as a whole.

That permission covers the headers only. The NDI SDK as a whole, and the NDI runtime these
headers describe, remain under the [NDI SDK License Agreement](http://ndi.link/ndisdk_license),
and Satellite ships **none** of it — the runtime is located and loaded at run time from the
user's own installation. See `docs/ARCHITECTURE.md` §12.

## Rules for this directory

- **Do not edit these files.** They are upstream verbatim. Anything Satellite needs on top of
  them belongs in `src/transport/ndi/`.
- `.clang-format` in this directory sets `DisableFormat: true`, because CI passes changed
  files to clang-format explicitly and would otherwise reformat upstream code.
- To update, replace the whole directory from a newer SDK and note the version below.

## Version

NDI 6 SDK headers (`NDILIB_REDIST_FOLDER` is `NDI_RUNTIME_DIR_V6`), struct `NDIlib_v6_3`.

NDI® is a registered trademark of Vizrt NDI AB. Satellite is not affiliated with or endorsed
by Vizrt.
