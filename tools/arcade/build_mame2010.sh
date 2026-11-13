#!/usr/bin/env bash
# Builds libmame2010_libretro_android.so (arm64-v8a) into corehost/libs/arm64-v8a.
#
# Why not the libretro buildbot .so: MAME 0.139 relies on `this == NULL` checks
# (src/emu/devintrf.h: `return (this) ? ... : ...;`). Modern clang deletes them, so the
# buildbot arm64 build crashes in machine_config::detokenize -> astring_cpy on every game.
# Fix: compile with -fno-delete-null-pointer-checks.
# Also: the jni/Android.mk recursive `uniq` overflows make.exe's stack on Windows -> use $(sort).
# On Windows the link line is too long for CreateProcess -> APP_SHORT_COMMANDS=true (response files).
#
# Usage: tools/arcade/build_mame2010.sh <work dir>   (needs git + Android NDK; takes a while)
set -euo pipefail
WORK="${1:?work dir}"
NDK="${ANDROID_NDK_HOME:-$LOCALAPPDATA/Android/Sdk/ndk/30.0.15729638}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

[ -d "$WORK/src" ] || git clone --depth 1 https://github.com/libretro/mame2010-libretro.git "$WORK/src"
cd "$WORK/src/jni"
sed -i 's/^COREFLAGS += -DCRLF=2/COREFLAGS += -fno-delete-null-pointer-checks -DCRLF=2/' Android.mk
sed -i 's/^SOURCES_UNIQ := \$(call uniq,\$(SOURCES_OBJ))/SOURCES_UNIQ := $(sort $(SOURCES_OBJ))/' Android.mk
grep -q fno-delete-null-pointer-checks Android.mk

NDK_BUILD="$NDK/ndk-build"; [ -f "$NDK_BUILD.cmd" ] && NDK_BUILD="$NDK_BUILD.cmd"
"$NDK_BUILD" APP_ABI=arm64-v8a APP_PLATFORM=android-24 APP_SHORT_COMMANDS=true -j"$(nproc)"

cp ../libs/arm64-v8a/libretro.so "$REPO_ROOT/corehost/libs/arm64-v8a/libmame2010_libretro_android.so"
echo "OK -> corehost/libs/arm64-v8a/libmame2010_libretro_android.so"
