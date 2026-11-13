#!/usr/bin/env bash
# Collects the source code of every core that ships in release builds, at the exact commit recorded
# in corehost/CORES_MANIFEST.json, as one .tar.gz per core plus SHA256SUMS. These archives are the
# "corresponding source" that goes next to the published core host source.
#
#   tools/cores/archive_sources.sh [output-dir]        (Linux / WSL; default: ~/rn/source-archives)
#
#   ONLY=mgba,fceumm   limit to some cores
#   WORK=<dir>         where clones are kept (default: ~/rn/src-fetch); safe to delete afterwards
#
# A core that was built on this machine is archived from its build clone (git archive of the pinned
# commit, plus a patch file if the tree was modified). Every other core is cloned from its upstream
# repository, checked out at the commit, with all submodules.
# melonDS DS fetches its dependencies at configure time instead of using submodules; they are taken
# from tools/cores/pins.json and placed under _retronest_deps/ inside its archive.
# The script only downloads source code and writes archives. It publishes nothing.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$HOME/rn/source-archives}"
WORK="${WORK:-$HOME/rn/src-fetch}"
ONLY="${ONLY:-}"
mkdir -p "$OUT" "$WORK"

# Build clones on this machine (id -> path). Used only when HEAD is the manifest commit.
declare -A LOCAL=(
  [bsnes]="$HOME/rn/cores/bsnes"
  [stella]="$HOME/rn/cores/stella"
  [melondsds]="$HOME/rn/cores/melondsds"
  [mame]="$HOME/rn/mame"
)

LIST="$WORK/cores.tsv"
python3 - "$REPO_ROOT" > "$LIST" <<'EOF'
import json, sys
root = sys.argv[1]
m = json.load(open(root + '/corehost/CORES_MANIFEST.json', encoding='utf-8'))
for c in m['cores']:
    if c.get('shippedInRelease') and c.get('upstream') and c.get('commit'):
        print(c['id'], c['upstream'], c['commit'], sep='\t')
EOF
DEPS="$WORK/melondsds-deps.tsv"
python3 - "$REPO_ROOT" > "$DEPS" <<'EOF'
import json, sys
p = json.load(open(sys.argv[1] + '/tools/cores/pins.json', encoding='utf-8'))
for name, d in p['cores']['melondsds'].get('deps', {}).items():
    print(name, d['repo'], d['commit'], sep='\t')
EOF

fail=0
note() { printf '%s\n' "$*"; }

# clone_at <dir> <repo> <commit>: full history without file contents, files of that commit only.
clone_at() {
  local d="$1" repo="$2" commit="$3" full api
  if [ ! -d "$d/.git" ]; then
    rm -rf "$d"
    git clone -q --filter=blob:none --no-checkout "$repo" "$d" || return 1
  fi
  if ! full=$(git -C "$d" rev-parse --verify -q "$commit^{commit}"); then
    # Not reachable from a branch or tag (or a short hash git cannot expand): ask GitHub for the full
    # hash and fetch that commit directly.
    api="https://api.github.com/repos/${repo#https://github.com/}/commits/$commit"
    full=$(curl -fsS -H "User-Agent: source-archive" "$api" |
      python3 -c 'import json,sys; print(json.load(sys.stdin)["sha"])') || return 1
    git -C "$d" fetch -q origin "$full" || return 1
    full=$(git -C "$d" rev-parse --verify -q "$full^{commit}") || return 1
  fi
  [ -n "$full" ] || return 1
  git -C "$d" checkout -q -f "$full" || return 1
  if ! git -C "$d" submodule update -q --init --recursive --depth 1 --jobs 4 2> "$d.submodule-errors"; then
    # Some upstream trees carry a submodule entry without a URL. Take what can be fetched and say so.
    git -C "$d" submodule update -q --init --depth 1 --jobs 4 2>> "$d.submodule-errors" || true
    echo "   submodule problems (recorded in the archive): $(head -1 "$d.submodule-errors")" >&2
  else
    rm -f "$d.submodule-errors"
  fi
  echo "$full"
}

while IFS=$'\t' read -r id repo commit; do
  if [ -n "$ONLY" ] && [[ ",$ONLY," != *",$id,"* ]]; then continue; fi
  note "== $id  $repo  $commit"
  stage="$WORK/stage/$id"
  rm -rf "$WORK/stage"; mkdir -p "$stage"
  info="$stage/RETRONEST_SOURCE_INFO.txt"
  local_dir="${LOCAL[$id]:-}"
  full=""
  if [ -n "$local_dir" ] && [ -d "$local_dir/.git" ] &&
     full=$(git -C "$local_dir" rev-parse --verify -q "$commit^{commit}") &&
     [ "$(git -C "$local_dir" rev-parse HEAD)" = "$full" ]; then
    how="build clone on the build machine (git archive of the commit)"
    git -C "$local_dir" archive --format=tar "$full" | tar -x -C "$stage"
    if [ -n "$(git -C "$local_dir" status --porcelain --untracked-files=no)" ]; then
      git -C "$local_dir" diff HEAD > "$stage/RETRONEST_LOCAL_CHANGES.patch"
      note "   tracked files differ from the commit: patch included"
    fi
    if [ -f "$local_dir/.gitmodules" ]; then
      note "   WARNING: build clone has submodules; they are not in a git archive"; fail=1
    fi
  else
    how="fresh clone of the upstream repository, submodules included"
    if ! full=$(clone_at "$WORK/clone/$id" "$repo" "$commit"); then
      note "   FAILED to fetch"; fail=1; continue
    fi
    tar -C "$WORK/clone/$id" --exclude=.git -cf - . | tar -x -C "$stage"
  fi
  {
    echo "Core: $id"
    echo "Upstream: $repo"
    echo "Commit: $full"
    echo "Taken from: $how"
    if [ -d "$WORK/clone/$id/.git" ] && [ "$how" != "${how#fresh}" ]; then
      echo "Submodules:"; git -C "$WORK/clone/$id" submodule status --recursive 2>/dev/null | sed 's/^/  /' || true
      if [ -f "$WORK/clone/$id.submodule-errors" ]; then
        echo "Submodule entries that could not be fetched from upstream (git's messages):"
        sed 's/^/  /' "$WORK/clone/$id.submodule-errors"
      fi
    fi
  } > "$info"

  if [ "$id" = "melondsds" ]; then
    echo "Dependencies fetched at configure time (tools/cores/pins.json), under _retronest_deps/:" >> "$info"
    while IFS=$'\t' read -r dname drepo dcommit; do
      note "   dep $dname $dcommit"
      if dfull=$(clone_at "$WORK/clone/melondsds-dep-$dname" "$drepo" "$dcommit"); then
        mkdir -p "$stage/_retronest_deps/$dname"
        tar -C "$WORK/clone/melondsds-dep-$dname" --exclude=.git -cf - . | tar -x -C "$stage/_retronest_deps/$dname"
        echo "  $dname  $drepo  $dfull" >> "$info"
      else
        note "   FAILED dep $dname"; fail=1
      fi
    done < "$DEPS"
  fi

  name="$id-${full:0:12}.tar.gz"
  rm -f "$OUT/$id-"*.tar.gz
  tar -C "$WORK/stage" --sort=name --owner=0 --group=0 --numeric-owner -czf "$OUT/$name" "$id"
  note "   -> $name  $(du -h "$OUT/$name" | cut -f1)  ($(find "$stage" -type f | wc -l) files)"
done < "$LIST"
rm -rf "$WORK/stage"

( cd "$OUT" && sha256sum ./*.tar.gz | sed 's# \./# #' > SHA256SUMS )
note "Archives in $OUT:"; ls -la "$OUT"
[ "$fail" = 0 ] || { note "FINISHED WITH ERRORS"; exit 1; }
note "ALL OK"
