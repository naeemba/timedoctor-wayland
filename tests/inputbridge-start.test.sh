#!/usr/bin/env bash
# Self-check for td-inputbridge-start.c: a process named timedoctor that loads
# the library starts the td-inputbridge next to it with its own pid; a helper
# with another name does not. A stand-in bridge records what it was given.
#   make test
set -euo pipefail
cd "$(dirname "$0")/.."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $1"; echo "got: $2"; exit 1; }

cp td-waylandshot.so "$work/"
printf '#!/bin/sh\necho "$1 $PPID" >> "%s/started"\n' "$work" > "$work/td-inputbridge"
chmod +x "$work/td-inputbridge"
# A real program, not a copy of sleep: coreutils may be one multi-call binary
# that dispatches on its own name.
printf '#include <unistd.h>\nint main(void) { usleep(500000); return 0; }\n' | cc -x c -o "$work/timedoctor" -
cp "$work/timedoctor" "$work/helper"

LD_PRELOAD="$work/td-waylandshot.so" "$work/timedoctor" &
owner=$!
LD_PRELOAD="$work/td-waylandshot.so" "$work/helper"
wait "$owner"

[[ $(cat "$work/started" 2>&1) == "$owner $owner" ]] \
  || fail "expected one bridge owned by and parented to $owner" "$(cat "$work/started" 2>&1)"
echo ok
