#!/usr/bin/env bash
# Self-check for `make install` / `make uninstall`, run against a throwaway
# DESTDIR. Needs the build dependencies, not Time Doctor or a Wayland session.
#   make test
set -euo pipefail
cd "$(dirname "$0")/.."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $1"; echo "got: $2"; exit 1; }
install_into() { make -s install DESTDIR="$work" PREFIX=/opt/td ICON=/opt/td/icon.png >/dev/null; }

lib=$work/opt/td/lib/td-waylandshot
installed_files=(td-waylandshot.so td-inputbridge timedoctor-wayland)
snapshot() { (cd "$lib" && stat -c %i:%Y "${installed_files[@]}"); }
entry=$work/opt/td/share/applications/timedoctor.desktop

# A launcher left as a symlink by an older install is replaced by a real file.
mkdir -p "$lib"
ln -s /nonexistent/timedoctor-wayland "$lib/timedoctor-wayland"
touch "$lib/kept-by-uninstall"

install_into
for file in "${installed_files[@]}"; do
  [[ -f $lib/$file && ! -L $lib/$file && -x $lib/$file ]] || fail "$file not installed as an executable file" "$(ls -la "$lib")"
done
grep -qx "Exec=/opt/td/lib/td-waylandshot/timedoctor-wayland --customuri=%u" "$entry" \
  || fail "desktop entry does not launch the installed launcher" "$(cat "$entry" 2>&1)"
grep -qx "Icon=/opt/td/icon.png" "$entry" || fail "desktop entry has the wrong icon" "$(cat "$entry")"

# Nothing changed, so nothing is replaced: a replaced td-inputbridge loses its setgid bit.
before=$(snapshot)
install_into
[[ $(snapshot) == "$before" ]] \
  || fail "second install replaced unchanged files" "$(ls -li --time-style=+%s "$lib")"

# Uninstall removes only what install put there.
make -s uninstall DESTDIR="$work" PREFIX=/opt/td
for file in "${installed_files[@]}" ../../share/applications/timedoctor.desktop; do
  [[ ! -e $lib/$file ]] || fail "uninstall left $file" "$(ls -la "$lib" "$(dirname "$entry")")"
done
[[ -e $lib/kept-by-uninstall ]] || fail "uninstall removed a file it did not install" "$(ls -la "$lib")"

echo ok
