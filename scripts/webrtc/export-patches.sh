#!/usr/bin/env bash
# Regenerates patches/ from the difference between upstream Sunshine and this fork, then checks that
# applying them to a clean upstream checkout reproduces the fork's tree exactly.
#
#   scripts/webrtc/export-patches.sh [upstream-ref]   (default: upstream/master)
set -euo pipefail

base="${1:-upstream/master}"
root="$(git rev-parse --show-toplevel)"
cd "$root"

declare -A topics=(
  [0001-core-per-session-packet-queues]="src/thread_safe.h src/globals.h src/video.cpp src/audio.cpp"
  [0002-core-webrtc-server-integration]="src/config.h src/config.cpp src/main.cpp src/nvhttp.cpp src/confighttp.cpp"
  [0003-build-libdatachannel]="cmake/"
  [0004-webrtc-module]="src/webrtc/"
  [0005-web-ui-tv-pairing-and-options]="src_assets/"
  [0006-tests]="tests/"
  [0007-docs]="README.md docs/"
)

rm -f patches/*.patch
mkdir -p patches
for name in "${!topics[@]}"; do
  # shellcheck disable=SC2086
  git diff "$base" HEAD -- ${topics[$name]} > "patches/$name.patch"
done

# Every change outside PATCHES.md, patches/ and this script must belong to a topic.
uncovered="$(git diff --name-only "$base" HEAD -- . ':!PATCHES.md' ':!patches/' ':!scripts/webrtc/' \
  ':!src/thread_safe.h' ':!src/globals.h' ':!src/video.cpp' ':!src/audio.cpp' \
  ':!src/config.h' ':!src/config.cpp' ':!src/main.cpp' ':!src/nvhttp.cpp' ':!src/confighttp.cpp' \
  ':!cmake/' ':!src/webrtc/' ':!src_assets/' ':!tests/' ':!README.md' ':!docs/')"
if [[ -n "$uncovered" ]]; then
  echo "Files changed outside every topic; add them to a topic:" >&2
  echo "$uncovered" >&2
  exit 1
fi

check="$(mktemp -d)"
trap 'git worktree remove --force "$check" >/dev/null 2>&1 || true' EXIT
git worktree add -q --detach "$check" "$base"
git -C "$check" apply --whitespace=nowarn "$root"/patches/*.patch
git -C "$check" add -A
fork="$(git rev-parse HEAD)"
if ! git -C "$check" diff --cached --quiet "$fork" -- . ':!PATCHES.md' ':!patches/' ':!scripts/webrtc/'; then
  echo "Applying patches/ to $base does not reproduce this fork:" >&2
  git -C "$check" diff --cached --stat "$fork" -- . ':!PATCHES.md' ':!patches/' ':!scripts/webrtc/' >&2
  exit 1
fi
echo "patches/ reproduces this fork on $base"
