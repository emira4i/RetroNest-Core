#!/usr/bin/env python3
"""Builds corehost/CORES_MANIFEST.json: ties each bundled Libretro core .so to its upstream source commit.

Usage: python tools/cores_manifest.py
Standard library only. Never modifies the .so files; only reads them.

Commit/version are taken from the library_version string embedded in the binary, and only when
exactly one distinct match is found. Otherwise commit stays null and the candidate strings seen
are listed in "notes". Nothing is guessed.

Cores listed in tools/cores/pins.json are built by tools/cores/build_cores.sh from a pinned upstream
commit: their commit is the pinned full SHA (commitSource "local-build"). OVERRIDES records a commit
that the generic patterns can't parse; it is accepted only when its proof string is in the binary.
"""
import datetime
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIBS = ROOT / "corehost" / "libs" / "arm64-v8a"
CATALOG = ROOT / "app" / "src" / "main" / "assets" / "cores_catalog.json"
OUT = ROOT / "corehost" / "CORES_MANIFEST.json"
PINS = ROOT / "tools" / "cores" / "pins.json"
SUFFIX = "_libretro_android.so"

SHIPPED = {"fceumm", "mgba", "bsnes", "clownmdemu", "gearsystem", "stella", "pcsx_rearmed",
           "melondsds", "mupen64plus_next_gles3", "ppsspp", "mame"}

# Upstream repos (each verified to exist with an HTTP request; see report). None = unverified.
UPSTREAM = {
    "fceumm": "https://github.com/libretro/libretro-fceumm",
    "mgba": "https://github.com/mgba-emu/mgba",
    "bsnes": "https://github.com/libretro/bsnes-libretro",  # buildbot's git.libretro.com/libretro/bsnes
    "snes9x": "https://github.com/libretro/snes9x",
    "clownmdemu": "https://github.com/Clownacy/clownmdemu-libretro",
    "gearsystem": "https://github.com/drhelius/Gearsystem",
    "genesis_plus_gx": "https://github.com/libretro/Genesis-Plus-GX",
    "stella": "https://github.com/stella-emu/stella",
    "pcsx_rearmed": "https://github.com/libretro/pcsx_rearmed",
    "melondsds": "https://github.com/JesseTG/melonds-ds",
    "mame": "https://github.com/libretro/mame",
    "mame2003_plus": "https://github.com/libretro/mame2003-plus-libretro",
    "mame2010": "https://github.com/libretro/mame2010-libretro",
    "mupen64plus_next_gles3": "https://github.com/libretro/mupen64plus-libretro-nx",
    "ppsspp": "https://github.com/hrydgard/ppsspp",
}

HASH = r"[0-9a-f]{7,40}"
# Per-core patterns matched against single printable strings (full match). Groups: version, commit.
PATTERNS = {
    "clownmdemu": rf"(v\d+(?:\.\d+)+) ({HASH})",
    "genesis_plus_gx": rf"(v\d+(?:\.\d+)+) ({HASH})",
    "snes9x": rf"(\d+(?:\.\d+)+) ({HASH})",
    "fceumm": rf"(\(SVN\)) ({HASH})",
    "mgba": rf"(\d+\.\d+-\d+-({HASH}))",
    "gearsystem": rf"\s*(\d+(?:\.\d+)+-\d+-g({HASH}))",
    "pcsx_rearmed": rf"(?:\w+ )*(r\d+\w*-\d+-g({HASH}))",
    "mupen64plus_next_gles3": rf"(\d+(?:\.\d+)*-\w+) ({HASH})",
}
# For hash-suffixed describe strings, group 2 is the commit; for "version commit" strings too.

LOCAL_BUILD = {
    "mame": dict(version="0.289", pattern=rf"0\.289 \(({HASH})\)",
                 note="Built locally from libretro/mame (MAME 0.289 driver subset) by "
                      "tools/arcade/build_mame_current.sh, which clones libretro/mame at HEAD "
                      "(--depth 1); the built-in version string carries the commit."),
    "mame2010": dict(version="0.139", pattern=rf"0\.139 ({HASH})",
                     note="Catalog marks this core source=build (built locally, not downloaded)."),
}

# Commits read by hand from a runtime version string the PATTERNS can't parse. "proof" must occur in the
# binary as one complete embedded string, otherwise the override is ignored (stale binary).
OVERRIDES = {
    "ppsspp": dict(commit="0acbc18", proof="0acbc18",
                   note="The core reports 'PPSSPP 0acbc18' at runtime (format 'PPSSPP %s' plus the "
                        "embedded string '0acbc18'). On hrydgard/ppsspp that is "
                        "0acbc18824a673a1780088775f49df6d4cf5f2ef (2026-11-06)."),
}

PIN_NDK = None
STR_RE = re.compile(rb"[\x20-\x7e]{4,200}")
IP_RE = re.compile(r"^\d+\.\d+\.\d+\.\d+$")
CAND_RE = re.compile(r"^(?:[A-Za-z][\w ]{0,20} )?v?\d+\.\d+(?:\.\d+)?(?:[_\-+]\w+)?$")


def iter_strings(data):
    for m in STR_RE.finditer(data):
        yield m.group().decode("ascii")


def analyse(core_id, data):
    """Returns (version, commit, commit_source, notes)."""
    pat_src = PATTERNS.get(core_id)
    local = LOCAL_BUILD.get(core_id)
    pat = re.compile(pat_src) if pat_src else None
    lpat = re.compile(local["pattern"]) if local else None
    matches = set()
    cands = []
    for s in iter_strings(data):
        if len(s) > 80:
            continue
        if lpat and (m := lpat.fullmatch(s)):
            matches.add((local["version"], m.group(1)))
        elif pat and (m := pat.fullmatch(s)):
            g = m.groups()
            matches.add((g[0], g[1]))
        elif len(s) <= 40 and CAND_RE.match(s) and not IP_RE.match(s) and s not in cands:
            cands.append(s)
    # pcsx_rearmed also embeds "rNN <hash>"; the describe form is the one we accept above.
    commits = {c for _, c in matches}
    if len(matches) == 1 or (matches and len(commits) == 1):
        version, commit = sorted(matches)[0]
        if local:
            return version, commit, "local-build", local["note"]
        return version, commit, "embedded-version-string", None
    if local:
        return local["version"], None, "local-build", local["note"]
    if len(commits) > 1:
        note = f"Ambiguous: several distinct matches {sorted(matches)}; not accepted."
    else:
        note = "No unambiguous commit in embedded strings."
    if cands:
        note += " Candidate version-like strings seen: " + ", ".join(repr(c) for c in cands[:8]) + "."
    return None, None, "unknown", note


def pinned(core_id, pin, data):
    """Returns (version, commit, commit_source, notes, extra) for a core built from pins.json."""
    commit = pin["commit"]
    note = (f"Built locally by tools/cores/build_cores.sh from {pin['repo']} at the commit pinned in "
            f"tools/cores/pins.json ({pin['method']}, {pin.get('ndk', PIN_NDK)}).")
    short = commit[:7]
    if re.search(rb"(?<![0-9a-f])" + short.encode() + rb"(?![0-9a-f])", data):
        note += f" The binary embeds the short commit {short} (checked)."
    elif pin.get("embedsCommit", True):
        sys.exit(f"error: {core_id}: the binary does not embed {short}; it was not built from the pin "
                 f"(still the buildbot binary?). Run tools/cores/build_cores.sh {core_id}.")
    else:
        note += " The binary carries no commit string, so the pin can't be checked against it."
    extra = {}
    if pin.get("deps"):
        extra["dependencies"] = [{"name": k, "repo": d["repo"], "commit": d["commit"]}
                                 for k, d in pin["deps"].items()]
    return pin.get("version"), commit, "local-build", note, extra


def overridden(core_id, data):
    o = OVERRIDES[core_id]
    if o["proof"] not in set(iter_strings(data)):
        return None
    return None, o["commit"], "embedded-version-string", o["note"], {}


def main():
    global PIN_NDK
    if not LIBS.is_dir() or not list(LIBS.glob("*.so")):
        sys.exit(f"error: no .so files in {LIBS}")
    catalog = {c["id"]: c for c in json.loads(CATALOG.read_text(encoding="utf-8"))["cores"]}
    pins_file = json.loads(PINS.read_text(encoding="utf-8"))
    pins, PIN_NDK = pins_file["cores"], pins_file["ndk"]
    cores = []
    for so in sorted(LIBS.glob("*.so")):
        name = so.name
        if not (name.startswith("lib") and name.endswith(SUFFIX)):
            continue
        core_id = name[3:-len(SUFFIX)]
        data = so.read_bytes()
        st = so.stat()
        result = None
        if core_id in pins:
            result = pinned(core_id, pins[core_id], data)
        elif core_id in OVERRIDES:
            result = overridden(core_id, data)
        if result is None:
            result = (*analyse(core_id, data), {})
        version, commit, source, notes, extra = result
        cat = catalog.get(core_id, {})
        entry = {
            "id": core_id,
            "file": name,
            "sizeBytes": st.st_size,
            "sha256": hashlib.sha256(data).hexdigest(),
            "modified": datetime.datetime.fromtimestamp(st.st_mtime, datetime.timezone.utc)
                        .strftime("%Y-%m-%dT%H:%M:%SZ"),
            "license": cat.get("license"),
            "commercialUse": cat.get("commercialUse"),
            "shippedInRelease": core_id in SHIPPED,
            "upstream": pins[core_id]["repo"] if core_id in pins else UPSTREAM.get(core_id),
            "version": version,
            "commit": commit,
            "commitSource": source,
        }
        if notes:
            entry["notes"] = notes
        entry.update(extra)
        cores.append(entry)
    cores.sort(key=lambda e: e["id"])
    manifest = {
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "abi": "arm64-v8a",
        "cores": cores,
    }
    OUT.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUT} ({len(cores)} cores)")


if __name__ == "__main__":
    main()
