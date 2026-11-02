#!/usr/bin/env bash
# Build and install into ~/.local (see README.md); safe to re-run, and worth
# re-running after every Time Doctor update, which rewrites the desktop entry.
# Never asks for sudo: the one root step, for td-inputbridge, is printed.
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d "$HOME/timedoctor" ]; then
  echo "Time Doctor not installed in ~/timedoctor, nothing to do" >&2
  exit 0
fi
make -s install PREFIX="$HOME/.local"
