# :corehost (GPL-2.0-or-later)

The RetroNest core host: it runs one emulation session in its own Android process (`:core`) so a crashing
or leaking core can never take the app down. It contains

- the native libretro bridge (`src/main/cpp`: core loading, GLES presentation, AAudio output, JNI),
- the emulation engine (`LibretroEmulationEngine`, rewind, fast forward, rumble, frame pacing, cheats),
- `CoreHostService`, the binder service that exposes it (`ICoreHost`, see `:corehost-api`),
- host-side texts (`res/values*/strings_corehost.xml`; en, tr, ru, de),
- the libretro cores themselves (`libs/arm64-v8a/lib*_libretro_android.so`, packaged as jniLibs).

The app does not compile against this module. It depends on it with `runtimeOnly` and talks to it only over the
generic binder protocol in `:corehost-api`. This module must never depend on `:app`; types both sides need are
copied here (`engine/CoreEngine.kt`, `engine/HostFiles.kt`, `engine/HostStrings.kt`), not shared.

## Building

Needs the Android SDK with NDK 30.0.15729638 and CMake 4.1.2 (see `build.gradle.kts`), then from the repo root:

    ./gradlew assembleDebug

`src/main/cpp/libretro.h` (the official header) and the cores are not committed:

- `scripts/setup-phase1.ps1` fetches `libretro.h`; `scripts/fetch-cores.ps1` downloads the prebuilt cores listed in the app's
  core catalog into `corehost/libs/arm64-v8a/`.
- Self-built cores (Linux / WSL, see the header of each script for what it needs):
  - `tools/arcade/build_mame2010.sh <work dir>` builds MAME 2010 into `corehost/libs/arm64-v8a/`.
  - `tools/arcade/build_mame_current.sh [work dir]` builds a driver subset of current MAME (GPL-2.0+); copy the output to
    `corehost/libs/arm64-v8a/libmame_libretro_android.so`.
  - `tools/cores/build_cores.sh [core id ...]` builds bsnes, Stella and melonDS DS from the upstream commits pinned in
    `tools/cores/pins.json` (GPL corresponding source); copy the outputs from `~/rn/cores/out/` here.
- `python tools/cores_manifest.py` regenerates `CORES_MANIFEST.json` (each core's upstream commit).
- `python tools/check_16kb.py` verifies every core is 16 KB page-size compatible (all `.so` files here are linked with
  `max-page-size=16384`).

The app module sets `useLegacyPackaging = true` so the `.so` files are extracted to `nativeLibraryDir`, where the host
`dlopen()`s them.

## Locale

Below Android 13 the host reads the app's language choice from SharedPreferences `retronest_locale` / `app_language`
(`HostLocale` in `engine/HostStrings.kt`, mirror of the app's `AndroidAppLocale`); on 13+ the platform per-app locale applies.
