# wmbench

Benchmarks and visual defect tests for window managers. Works on any window
manager, X11 or Wayland: it measures the session as it is, so two desktops can
be put side by side.

    make                    build everything
    ./benchmark.sh          measure the session, print a table
    ./validate.sh           look for visual defects, print a verdict
    ./compare_results.sh    put the saved runs side by side

Both entry points build what they need, work out which compositor is running,
and save their table under `results/`. Whichever window manager is running says
so itself, so replacing one mid-session is noticed. A column that cannot be
measured reads `-`: power with no sensor, compositor CPU with no visible
process.

## The two backends

Every binary carries both and picks at startup: `WAYLAND_DISPLAY` set means
native Wayland, otherwise Xlib. `BENCH_BACKEND=x11` forces the X11 backend on a
Wayland session, so it runs through XWayland - the same load twice is how
XWayland's own cost is measured. `BENCH_OUTPUT=<name>` picks the monitor.

What runs natively depends on the protocols the compositor speaks, probed at
startup:

| protocol | gives | spoken by |
| --- | --- | --- |
| wlr-layer-shell | putting a window at a screen coordinate | KWin, cosmic-comp, labwc, sway, hyprland - not mutter |
| xx-zones (experimental) | placing and moving managed toplevels, positions reported back | KWin so far |
| xdg-activation | raising and focusing own windows | everyone |
| wp-alpha-modifier | whole-window opacity | KWin, sway, hyprland, labwc - not mutter |
| xdg-shell | maximize, minimize, fullscreen, popups | everyone |
| xdg-decoration | a compositor-drawn frame, as every X11 window gets | most - not mutter |
| wlr-foreign-toplevel | what a taskbar knows: other windows, unminimise | wlroots compositors - not KWin, not mutter |

Where a protocol is missing, the usual refusals fire - `PLACE-IGNORED`,
`MOVE-REFUSED`, exit 3 - and the row reads "not done" instead of a number for
work that never happened. On GNOME that is every test built out of placed
windows.

Some Wayland rows measure a differently-shaped ask:

- **move** travels a layer surface by its own margins: real compositing, but no
  frame being moved. A screenshot proves it before and after the measurement,
  never inside; without a screenshot tool the row says its moves are unproven.
- **windows** walks the same states through xdg-shell, checked against the
  compositor's configure events. Zones make the walks real moves; elsewhere
  nothing a client says moves a managed toplevel, so the twelve steps to each
  edge are carried by the size instead - the same count of composited steps.
  Unminimise goes through foreign-toplevel where it exists and an
  xdg-activation token otherwise; neither, and the row exits 3.
- **zone placement** reports positions back, so those rows need no screenshot.
  It deliberately will not say where a window sits on screen, so pixel checks
  never aim at one.
- **resize, scroll and dnd** place their own two windows, undecorated, on X11
  too. Nothing else gives both sessions one scene: a Wayland compositor puts a
  managed window where it likes, and labwc put both in the same spot, covering
  one whole. The cost is no frame around them, and results from before this do
  not compare. **windows** keeps its managed pair, because states belong to
  one, so it is the one scene the two sessions still differ on.
- **transbench** sets `_NET_WM_WINDOW_OPACITY` blind on X11; on Wayland a
  compositor without alpha-modifier answers no and the row is "not done".
- **fullscreen asked** sets `_NET_WM_BYPASS_COMPOSITOR` on X11; Wayland has no
  such property, so it declares the surface opaque instead, which is what makes
  it eligible to go straight to the screen.

Fractional scaling breaks the 1:1 map between a window's pixels and a
screenshot's, so pixel checks answer "could not look" under it; integer scales
work. `BENCH_CAPTURE_CMD` must write the chosen output alone, which `grim` does
on a single-monitor desktop.

## benchmark.sh

Every workload does a **fixed amount of work**, so two sessions compare
directly and the time it took is a result rather than a setting. The frame rate
is the exception: flat out, windowed, once, last.

The fullscreen pair - the same frames as they come, then asking compositing to
step aside - is read in power, not frames, because the saving is the
compositor's work and not the application's.

No workload repeats another: the scripted person is split into `windows`
(maximize, minimize, snap, raise, fullscreen) and `scroll`, beside `resize` and
`dnd`.

Every load says where its windows landed, and the moving tests check they
really moved. A compositor that refuses - cosmic-comp will not reposition an
X11 window it manages, mutter will not place a Wayland one at all - leaves the
row empty and named at the end, rather than filled with the small number a
compositor doing nothing produces.

The CPU column is the window manager's own process. On X11 part of the
compositing happens inside the X server and is not counted; the server also
does every program's drawing, which on Wayland happens inside the programs, so
counting it would tilt the comparison the other way. Read X11 numbers against
X11 ones.

## validate.sh

Nine tests, one visual defect each. Each logs the geometry it asked for against
what it got and photographs the window's own content, so a compositor that
quietly ignores a move is caught. The screen has to be idle, or the tests might
fail.

Each answers with one of four statuses: **passed**, **failed** (the compositor
is at fault), **could not run** (no display, no way to photograph the screen)
and **not available** (the compositor does not do the thing being tested).

Wayland needs a screenshot tool: `grim`, or `gnome-screenshot` or `spectacle`
together with `ffmpeg` to convert their PNG. Without one the pixel tests are
skipped. Two "not available" verdicts are Wayland's own rather than any
compositor's fault: `shape_check`, because there is no shape concept there (the
defect it hunts lives in per-pixel alpha, argbbench's ground), and
`leftover_check`, because no protocol tells one window's place from another's.
The rest run natively on layer-shell compositors.

Corruption produced after the framebuffer, by colour compression for instance,
is on the panel but in no capture, so a clean verdict does not cover it.

| test | the defect it catches |
| --- | --- |
| `motion_check` | tearing: a capture taken while a pattern scrolls that is not one single frame |
| `stale_check` | staleness: leftovers after scrolling stops, and a window nobody draws to while another is hammered |
| `pop_check` | what a menu covered not coming back when it closes |
| `suspend_check` | the screen not coming back after compositing suspends for a fullscreen window |
| `shape_check` | a non-rectangular window painting its undefined corners over what is behind it (X11 only) |
| `resize_check` | a frame drawn from a window pixmap that was not ready yet, during continuous resizing |
| `offscreen_check` | a window hanging off the left or top edge showing the wrong part of itself |
| `iconify_check` | a window not coming back correctly after being minimised |
| `leftover_check` | garbage left on the desktop by the load, by comparing the idle screen before it with the same screen after (X11 only) |

The stability pass runs everything at once, started and finished together, with
`motion_check`'s pattern scrolling in the middle: the same question asked of a
busy compositor. Windows open one at a time, each waited for, so they stack in
open order and every session composites the same scene. X11 asks the server for
the order; Wayland cannot read it back, so each tool prints `WINDOW-UP` after
its first commit and the report says the order is unverified.

## The power metric

The following sensors are supported:

- CPU and Intel integrated GPU - `energy_uj`
- Intel discrete GPU - `energy1_input`
- AMD discrete/integrated GPU - `power1_average`
- NVIDIA discrete GPU - `nvidia-smi`, present in NVIDIA driver only, so mesa
  drivers won't read it

When an integrated GPU is used, the power reading will combine CPU and GPU into
a single value.

The CPU sensor might be root-only on some machines. In this case, you are asked
whether to open it with root, before anything is measured.
