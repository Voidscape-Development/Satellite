# Releasing Satellite

## What CI produces

Every push and pull request builds Satellite for Windows, Linux and macOS. Tagged pushes
also package it, producing the installers and archives the OBS plugin template defines:

| Platform | Artifact | Contents |
|---|---|---|
| Windows | `satellite-<version>-windows-x64.zip` and an NSIS installer | `satellite.dll`, `libomt.dll`, `libvmx.dll`, `data/` |
| Linux | `.deb` and a tarball | `satellite.so`, `libomt.so`, `libvmx.so` under `obs-plugins`, `data/` under `share/obs` |
| macOS | `satellite-<version>-macos-universal.pkg` | `satellite.plugin`, with both OMT libraries inside `Contents/MacOS` |

The OMT libraries are built from source by `build-aux/build-omt` before the plugin is
configured, and CMake installs them beside the plugin binary. If that step is skipped the
package is still valid — OMT just reports itself unavailable at run time.

**The NDI runtime is never included.** It is proprietary and not redistributable. Users
install it themselves, and the Satellite window links them to it.

## Releases are unsigned

Satellite has no code signing certificates, so **nothing in a release is signed or
notarized**. That is a deliberate position, not an oversight, and it has consequences worth
stating in release notes:

- **macOS** builds are ad-hoc signed (identity `-`), which is what the template does without
  a certificate. Gatekeeper will refuse the package on first open; users need
  *System Settings → Privacy & Security → Open Anyway*, or `xattr -dr com.apple.quarantine`
  on the downloaded file.
- **Windows** builds are unsigned, so SmartScreen shows "Windows protected your PC" until
  enough people run it. *More info → Run anyway*.
- **Linux** packages are unsigned, which is normal for out-of-repo `.deb` files.

The workflow already handles the absence of certificates: `setup-macos-codesigning` reports
`haveCodesignIdent: false`, packaging skips the signing step, and the build falls back to
ad-hoc signing. Nothing needs disabling. If certificates are obtained later, adding these
repository secrets is all that is required — no workflow changes:

```
MACOS_SIGNING_APPLICATION_IDENTITY   MACOS_SIGNING_INSTALLER_IDENTITY
MACOS_SIGNING_CERT                   MACOS_SIGNING_CERT_PASSWORD
MACOS_KEYCHAIN_PASSWORD              MACOS_SIGNING_PROVISIONING_PROFILE
MACOS_NOTARIZATION_USERNAME          MACOS_NOTARIZATION_PASSWORD
```

## Cutting a release

1. **Bump the version** in `buildspec.json`. It is the single source of truth — CMake reads
   it, and the packaging scripts name their artifacts from it.
2. **Check the OMT pins** in the same file. `libomtnetCommit`, `libomtCommit` and
   `libvmxCommit` decide what gets built and shipped. Bumping them is a deliberate act, and
   the build should be run locally before trusting a new pin.
3. **Run the tests** — they are off by default:
   ```sh
   cmake -S . -B build -DENABLE_TESTS=ON && cmake --build build
   ctest --test-dir build --output-on-failure
   ```
4. **Tag and push.** The workflow packages on tags:
   ```sh
   git tag -a 0.2.0 -m "Satellite 0.2.0" && git push origin 0.2.0
   ```
5. **Write the release notes**, including the unsigned-build note above, the NDI runtime
   requirement, and — for Linux — the Avahi requirement for OMT.

## Things that will trip you up

These are all real, and each cost a CI round trip to find:

- **`CMakePresets.json` overrides `CMakeLists.txt`.** The hidden `template` preset sets cache
  variables that every other preset inherits, and a preset cache variable beats an
  `option()` default. `ENABLE_QT` and `ENABLE_FRONTEND_API` are set there, not in
  `CMakeLists.txt`.
- **Nothing may write into `.deps/`.** The macOS configure runs
  `xattr -r -d com.apple.quarantine .deps` with `COMMAND_ERROR_IS_FATAL`, so a single
  unreadable file there fails the whole build. Git pack files are read-only, which is why
  `build-aux/build-omt` keeps its clones outside the source tree entirely.
- **No semicolons in `buildspec.json`.** `cmake/common/bootstrap.cmake` passes the file to
  `string(JSON ...)` unquoted, so a semicolon anywhere splits it into a CMake list and every
  lookup in it fails with a confusing `VERSION "NOTFOUND"` error.
- **`plugin-support.h` must not declare `blogva`.** libobs declares it with `EXPORT`, which
  is `__declspec(dllimport)` on MSVC, so a bare declaration in a header makes every C++ file
  that also includes the obs headers fail with "redefinition; different linkage". The
  declaration lives in `plugin-support.c.in`, which never sees those headers.
