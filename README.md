# RetroNest core host

The open-source part of RetroNest: the service that loads Libretro cores in its own process and the
small protocol module the app uses to talk to it.

| Module | License | Contents |
|---|---|---|
| `corehost` | GPL-2.0-or-later (`LICENSE`) | Core host service, emulation loop, JNI bridge (`src/main/cpp`) |
| `corehost-api` | MIT (`corehost-api/LICENSE`) | AIDL interfaces and protocol constants |

The RetroNest app itself is a separate program that communicates with the core host only through
the binder interface in `corehost-api`.

This tree was exported from RetroNest commit `7820030bad59bd4ea8cc4e0bf9208920b142a52a`.

## Cores

The cores and the commits they were built from are listed in [SOURCES.md](SOURCES.md).
No core binary is part of this tree. To get them:

- `scripts/fetch-cores.ps1` downloads the prebuilt cores and `libretro.h`.
- `tools/cores/build_cores.sh` builds bsnes, Stella and melonDS DS from the commits in
  `tools/cores/pins.json` (Linux, Android NDK).
- `tools/arcade/build_mame_current.sh` builds the arcade core (Linux, Android NDK).

Binaries go to `corehost/libs/arm64-v8a/`.

The source code of each shipped core at its exact commit is published as one archive per core next
to this tree (collected with `tools/cores/archive_sources.sh`).

## Build

Requirements: JDK 17 or newer, Android SDK with the NDK and CMake versions named in
`corehost/build.gradle.kts`, and a `local.properties` with `sdk.dir`.

```
./gradlew :corehost:assembleRelease
```

The result is an Android library (AAR) containing the host service, the native bridge and whichever
cores are in `corehost/libs/arm64-v8a/`.

## Tests

`./gradlew :corehost:connectedDebugAndroidTest` starts ten cores on a connected arm64 device with
small generated test programs (`tools/testroms/make_test_roms.py`). No game files are needed.

RetroNest does not include or provide any games or system files.
