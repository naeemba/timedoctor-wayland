# timedoctor-wayland

Makes the Time Doctor Linux desktop app track correctly under Hyprland
(Wayland).

> **Disclaimer.** This is an unofficial, independent project. It is not
> affiliated with, endorsed by or supported by Time Doctor. It contains no Time
> Doctor code, binaries or assets; it is a set of small helpers that run beside
> the app you installed yourself. It only reports real activity and real screen
> content: screenshots are real captures of your screen, activity is counted
> only from real key presses and pointer movement, the active window is the one
> you actually focused, and Meeting time is counted only while an app really
> records from your microphone. It does not fake work, generate input on its
> own or hide anything from the tracker. Check your employer's policy before
> using it.

## The problem

Time Doctor's Linux app is written for X11. On a Wayland compositor it runs
under XWayland, which breaks almost everything it tracks:

- **Screenshots are black.** It grabs the X11 root window, and XWayland's root
  window holds no Wayland content.
- **Activity reads as idle.** It counts XI2 raw events on the X11 root window,
  which only fire for input sent to X11 windows. Typing into a native Wayland
  app counts for nothing.
- **App tracking records Time Doctor itself.** It reads `_NET_ACTIVE_WINDOW`,
  which only knows X11 windows.
- **3.20 picks its Wayland backends**, which expect GNOME Shell, and then
  leaves mouse movement uncounted.
- **No microphone signal.** The Windows and macOS builds count Meeting time
  while the microphone is in use and do not stop the timer at the idle check
  during a call; the Linux build has neither.

## How each piece works

Four sources are built into one `LD_PRELOAD` library, `td-waylandshot.so`; a
small program and a launcher run beside it.

- `td-waylandshot.c` hooks the xcb calls Qt uses to grab the root window
  (`xcb_copy_area`, `xcb_get_image`, `xcb_get_image_reply`, `xcb_image_get`)
  and fills the returned pixels with a real capture from `grim`. X monitors
  (from `xrandr --listmonitors`) are matched to Wayland outputs by name, so
  each monitor is captured and placed separately.
- `td-activewindow.c` answers `_NET_ACTIVE_WINDOW`, `_NET_WM_NAME` and
  `_NET_WM_PID` with the focused window from Hyprland's IPC socket
  (`activewindow`) when that window is native Wayland. Requests from Qt and
  for focused XWayland windows pass through untouched.
- `td-sessiontype.c` answers `x11` to the one
  `loginctl show-session … -p Type` query 3.20+ uses to choose its backends,
  so it stays on the X11 paths the rest of this project makes work. Every
  other `popen()` passes through.
- `td-microphone.c` polls PipeWire through `pactl` every 5 seconds for apps
  recording from a real microphone (monitor sources and paused streams
  excluded; no audio is read). When one starts or stops, it emits the app's
  own `audioCaptureChanged` Qt signal, which Time Doctor records as Meeting
  time, and while one is recording it makes the "user is on a call" check
  answer yes, so the idle check does not stop the timer.
- `td-inputbridge.c` reads the real keyboard, mouse and touchpad devices in
  `/dev/input` and mirrors each real event into XWayland through XTest: a key
  press becomes a press of a keycode with no keysym (it types nothing), and
  motion or a click warps the X pointer to the real cursor position from
  Hyprland. A click warps only once the button is back up, and nothing moves
  the X pointer while a mouse or pen button is held, or a pen tip or finger
  is down on a touchscreen or pen tablet, so clicks and drags in X apps are
  not turned into tiny drags. A finger resting on a touchpad does not hold
  the pointer. Nothing is sent without real input, and key
  codes are never logged. It exits a minute after Time Doctor quits.
- `timedoctor-wayland` is the launcher: it starts `td-inputbridge`, preloads
  the library, forces Qt's xcb platform and hides `WAYLAND_DISPLAY` from Time
  Doctor (passing it on to `grim` as `TD_WAYLAND_DISPLAY`).

## Supported Time Doctor versions

Developed and tested against **3.20.14**, the Linux build installed in
`~/timedoctor` by Time Doctor's own installer.

- Screenshots, app tracking and the input bridge use the X11 code paths and do
  not depend on one release.
- The session-type answer only matters on 3.20 and later; earlier releases
  already use X11.
- The **on-call detour** is tied to 3.20.14: it patches a function at a fixed
  offset in `libtimedoctor.so`, and only after checking that the function's
  first bytes match 3.20.14 exactly. On any other version it skips itself and
  logs `on-call check not at the 3.20.14 offset; timer detour skipped`.
  Meeting time (`audioCaptureChanged`) is found by name and does not depend on
  the version.

## Requirements

- Hyprland (the active window and the cursor position come from its IPC
  socket)
- Time Doctor installed in `~/timedoctor`
- `grim`, `xrandr` and `pactl` on `PATH`
- 3.20+ also needs the `webkit2gtk-4.1` package
- To build: a C compiler, `make`, and the headers for libxcb, xcb-util-image,
  libX11 and libXtst

On Arch: `pacman -S grim xorg-xrandr libpulse webkit2gtk-4.1 libxcb xcb-util-image libx11 libxtst`.

## Install

```sh
git clone https://github.com/naeemba/timedoctor-wayland
cd timedoctor-wayland
./install.sh
```

`install.sh` does nothing if Time Doctor is not in `~/timedoctor`; otherwise it
runs `make install PREFIX=~/.local`, which:

- installs `td-waylandshot.so`, `td-inputbridge` and the launcher into
  `~/.local/lib/td-waylandshot`;
- writes `~/.local/share/applications/timedoctor.desktop`, replacing Time
  Doctor's own menu entry with one that starts the launcher.

A file whose content is unchanged is left in place, and a changed one is
written beside the old and renamed over it, so re-running is safe while Time
Doctor is open. Time Doctor's installer and updates rewrite the desktop entry
to start the app directly: **re-run `./install.sh` after every update.**

`make install` takes `PREFIX` (default `~/.local`), `DESTDIR` and `ICON` (the
menu icon, default `~/timedoctor/timedoctor.png`).

### The input step

`td-inputbridge` must read `/dev/input`, which belongs to the `input` group.
The AUR package does this itself (see [Packaging](#packaging)). `make install`
never runs `sudo`; it prints this instead, to run once by hand:

```sh
sudo chgrp input ~/.local/lib/td-waylandshot/td-inputbridge
sudo chmod 2750 ~/.local/lib/td-waylandshot/td-inputbridge
```

The mode is 2750, not 2755, so only your account can run it: another account
could otherwise point it at its own X server and receive the timing of every
key you press. A reinstall that changes `td-inputbridge` replaces the file and
drops the bit; `make install` prints the step again when that happens.

## Logs and debugging

- Every part logs to `~/.cache/td-waylandshot.log` (set `TD_SHOT_LOG` to move
  it): each substituted screenshot, grim failures, microphone starts and stops,
  whether the on-call detour was installed, and the input bridge's device
  count.
- `TD_SHOT_DEBUG=<directory>` also writes every substituted screenshot there
  as a PPM image.
- `TIMEDOCTOR_DIR` points the launcher at a Time Doctor installed somewhere
  other than `~/timedoctor`.

## Uninstall

```sh
make uninstall PREFIX=~/.local
```

This removes the three installed files and the desktop entry. Time Doctor's
own entry comes back with its next update or reinstall.

## Packaging

`packaging/aur` holds a `PKGBUILD` that installs into `/usr`. Installing the
package needs no manual step: on every install and upgrade it gives
`/usr/lib/td-waylandshot/td-inputbridge` the `input` group and mode 2755.

That is 2755, not the user install's 2750, because the packaged file belongs to
root and 2750 would stop your account from running it. Any local account can
therefore run it and point it at its own X server. That account receives the
timing of every key you press, and because `td-inputbridge` parses the X
server's replies while it holds the `input` group, a libX11 reply-parsing bug
would hand it your real key codes too. On a machine shared with accounts you do
not trust, use the user install above instead.

A system-wide menu entry is shadowed by the one Time Doctor's installer writes
into `~/.local/share/applications`; delete that one, or use the user install
above.

## License

MIT, see [LICENSE](LICENSE).
