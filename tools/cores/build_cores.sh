#!/usr/bin/env bash
# Builds the Libretro cores RetroNest compiles itself (GPL corresponding source) for Android arm64-v8a,
# each from the upstream commit pinned in tools/cores/pins.json.
# Linux / WSL only (same setup as tools/arcade/build_mame_current.sh).
#
# Needs: build-essential git curl unzip python3 (all already needed by the MAME build).
#        CMake >= 3.19 for melondsds: the system cmake is used when present (apt install cmake);
#        otherwise the official CMake 3.31.6 release is downloaded into <work dir>/cores/tools and its
#        SHA-256 checked. Ninja is not needed (Unix Makefiles generator).
# Usage: tools/cores/build_cores.sh [core id ...]        (default: every core in pins.json)
#        WORK=<dir> (default ~/rn)  JOBS=<n> (default 4)  OUT_DIR=<dir> (default <work dir>/cores/out)
# Output: $OUT_DIR/lib<id>_libretro_android.so, stripped, 16 KB page aligned. Copy them to
#         corehost/libs/arm64-v8a/ and run tools/check_16kb.py and tools/cores_manifest.py.
# Sources: <work dir>/cores/<id> (a git checkout at the pinned commit; melondsds' fetched dependencies
#          end up in <work dir>/cores/melondsds/build/_deps, each pinned in pins.json too).
set -euo pipefail
export GIT_TERMINAL_PROMPT=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PINS="$SCRIPT_DIR/pins.json"
WORK="${WORK:-$HOME/rn}"
JOBS="${JOBS:-4}"
CORES_DIR="$WORK/cores"
OUT_DIR="${OUT_DIR:-$CORES_DIR/out}"
CMAKE_VERSION=3.31.6
CMAKE_SHA256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf

pin() { # pin <core id> <python expression on c (the core's entry)>
    python3 -c 'import json,sys; c=json.load(open(sys.argv[1]))["cores"][sys.argv[2]]; v=eval(sys.argv[3]); print(" ".join(v) if isinstance(v, list) else v)' "$PINS" "$1" "$2"
}

DEFAULT_NDK=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ndk"])' "$PINS")
mkdir -p "$WORK" "$CORES_DIR" "$OUT_DIR"
PAGE_FLAGS="-Wl,-z,max-page-size=16384"

use_ndk() { # use_ndk <android-ndk-rNN>: sets NDK and STRIP, downloading the NDK into $WORK if missing
    NDK="$WORK/$1"
    if [ ! -d "$NDK" ]; then
        (cd "$WORK" && curl -fsSL -o ndk.zip "https://dl.google.com/android/repository/$1-linux.zip" \
            && unzip -q ndk.zip && rm ndk.zip)
    fi
    STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
}

checkout() { # checkout <dir> <repo> <commit>: a git tree at exactly <commit> (shallow)
    local dir=$1 repo=$2 commit=$3
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$repo"
    fi
    git -C "$dir" remote set-url origin "$repo"
    if [ "$(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$commit" ]; then
        git -C "$dir" fetch -q --depth 1 origin "$commit"
        git -C "$dir" checkout -q --force FETCH_HEAD
    fi
    [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || { echo "checkout of $commit failed in $dir" >&2; exit 1; }
}

ensure_cmake() {
    if command -v cmake >/dev/null && python3 -c 'import sys; v=tuple(map(int, sys.argv[1].split(".")[:2])); sys.exit(v < (3, 19))' \
            "$(cmake --version | head -1 | awk '{print $3}')"; then
        CMAKE=cmake
        return
    fi
    local dir="$CORES_DIR/tools/cmake-$CMAKE_VERSION-linux-x86_64"
    if [ ! -x "$dir/bin/cmake" ]; then
        local tgz="$CORES_DIR/tools/cmake.tar.gz"
        mkdir -p "$CORES_DIR/tools"
        curl -fsSL -o "$tgz" "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
        echo "$CMAKE_SHA256  $tgz" | sha256sum -c -
        tar -xzf "$tgz" -C "$CORES_DIR/tools"
        rm "$tgz"
    fi
    CMAKE="$dir/bin/cmake"
}

build_ndk() { # ndk-build, as the libretro android-jni CI template does, plus 16 KB pages
    local id=$1 src=$2
    local jni; jni=$(pin "$id" 'c["jniDir"]')
    rm -rf "$src/$(dirname "$jni")/libs" "$src/$(dirname "$jni")/obj"
    "$NDK/ndk-build" --no-print-directory -j"$JOBS" -C "$src/$jni" \
        APP_ABI=arm64-v8a APP_PLATFORM=android-21 APP_SUPPORT_FLEXIBLE_PAGE_SIZES=true APP_LDFLAGS="$PAGE_FLAGS"
    BUILT="$src/$(dirname "$jni")/libs/arm64-v8a/libretro.so"
}

build_cmake() { # CMake Release build, as the libretro android-cmake CI template does, plus 16 KB pages
    local id=$1 src=$2
    ensure_cmake
    local target api args cxxflags deps=()
    target=$(pin "$id" 'c["target"]')
    api=$(pin "$id" 'c.get("apiLevel", 21)')
    args=$(pin "$id" 'c.get("cmakeArgs", [])')
    cxxflags=$(pin "$id" 'c.get("cxxFlags", "")')
    # Every fetched dependency at its pinned commit (pins.json "deps": cache variable prefix -> repo, commit).
    while read -r var repo commit; do
        [ -n "$var" ] && deps+=("-D${var}_REPOSITORY_URL=$repo" "-D${var}_REPOSITORY_TAG=$commit")
    done < <(pin "$id" '"\n".join(" ".join([k, d["repo"], d["commit"]]) for k, d in c.get("deps", {}).items())')
    # shellcheck disable=SC2086
    CXXFLAGS="$cxxflags" "$CMAKE" -S "$src" -B "$src/build" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$api" -DANDROID_STL=c++_static \
        -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
        -DCMAKE_SHARED_LINKER_FLAGS="$PAGE_FLAGS" -DCMAKE_MODULE_LINKER_FLAGS="$PAGE_FLAGS" \
        "${deps[@]}" $args
    "$CMAKE" --build "$src/build" --target "$target" -j"$JOBS"
    BUILT=$(find "$src/build" -name "${target}*.so" -type f | head -1)
    # Record what was actually fetched (each must equal its pin; also the corresponding-source list).
    for d in "$src"/build/_deps/*-src; do
        [ -d "$d/.git" ] && echo "  dep $(basename "$d" -src) $(git -C "$d" remote get-url origin) $(git -C "$d" rev-parse HEAD)"
    done
}

IDS=("$@")
[ ${#IDS[@]} -gt 0 ] || mapfile -t IDS < <(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["cores"]))' "$PINS")

for id in "${IDS[@]}"; do
    repo=$(pin "$id" 'c["repo"]')
    commit=$(pin "$id" 'c["commit"]')
    method=$(pin "$id" 'c["method"]')
    use_ndk "$(pin "$id" "c.get('ndk', '$DEFAULT_NDK')")"
    src="$CORES_DIR/$id"
    echo "==> $id: $repo @ $commit ($method, $(basename "$NDK"))"
    start=$SECONDS
    checkout "$src" "$repo" "$commit"
    case "$method" in
        ndk-build) build_ndk "$id" "$src" ;;
        cmake) build_cmake "$id" "$src" ;;
        *) echo "unknown build method $method" >&2; exit 1 ;;
    esac
    out="$OUT_DIR/lib${id}_libretro_android.so"
    "$STRIP" --strip-unneeded -o "$out" "$BUILT"
    echo "==> $id built in $((SECONDS - start)) s: $out ($(stat -c %s "$out") bytes)"
done
