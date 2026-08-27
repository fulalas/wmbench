# wmbench

Benchmarks and visual defect tests for window managers, X11 or Wayland. It
measures the session as it is, so two desktops can be put side by side. What a
window manager cannot do is reported as not done rather than as a number.

    make                    build everything
    ./benchmark.sh          measure the session, print a table
    ./benchmark.sh -x11     the same, with the windows opened through XWayland
    ./validate.sh           look for visual defects, print a verdict
    ./compare_results.sh    put the saved runs side by side
    ./compare_percent.sh    the same, as percentages of the best

Both entry points build what they need and save their table in the results
folder. Which window manager is running is found by asking the session, not by
matching a list of names, so an unknown compositor still runs. A column that
cannot be measured reads `-`: power with no sensor, desktop CPU with no
visible process.

## The two backends

Every binary carries both and picks at startup: `WAYLAND_DISPLAY` set means
native Wayland, otherwise Xlib. `BENCH_BACKEND=x11`, or `benchmark.sh -x11`,
forces the X11 backend on a Wayland session, so it runs through XWayland and is
recorded as `xwayland`. `BENCH_OUTPUT=<name>` picks the monitor.

What runs natively depends on the protocols the compositor speaks, probed at
startup:

| protocol | gives | spoken by |
| --- | --- | --- |
| wlr-layer-shell | putting a window at a screen coordinate | KWin, cosmic-comp, labwc, sway, hyprland - not mutter |
| xx-zones (experimental) | placing and moving managed toplevels, positions reported back | nobody yet - KWin 6.7.4 does not offer it |
| xdg-activation | raising and focusing own windows | everyone |
| wp-alpha-modifier | whole-window opacity | KWin, sway, hyprland, labwc - not mutter |
| xdg-shell | maximize, minimize, fullscreen, popups | everyone |
| xdg-decoration | a compositor-drawn frame, as every X11 window gets | most - not mutter |
| wlr-foreign-toplevel | what a taskbar knows: other windows, unminimise | wlroots compositors - not KWin, not mutter |

Where a protocol is missing the test reads "not done" instead of a number. On
GNOME that is every test built out of placed windows.

Some Wayland tests measure a differently-shaped ask:

- **move** travels a layer surface by its margins, so no frame is moved. A
  screenshot proves it; without a screenshot tool the moves are unproven.
- **windows** walks states only, and leaves the window minimized, because no
  protocol lets a client bring one back.
- **resize and scroll** place their own two windows, undecorated, on X11 too,
  so both sessions get one scene. The cost is no frame around them.
- **dnd** puts both views and the icons inside one window, so it runs where
  windows cannot be placed at all.
- **stress** is "not done" without alpha-modifier: its translucent window is
  part of the mix.
- **fullscreen asked** declares the surface opaque instead of setting
  `_NET_WM_BYPASS_COMPOSITOR`.

Results saved before these changes do not compare on `windows`, `resize` and
`scroll`.

Fractional scaling breaks the 1:1 map between a window's pixels and a
screenshot's, so pixel checks answer "could not look" under it; integer scales
work. `BENCH_CAPTURE_CMD` must write the chosen output alone, which `grim` does
on a single-monitor desktop.

## benchmark.sh

Every workload does a **fixed amount of work**, so two sessions compare
directly. The frame rate is the exception: flat out, windowed, once, last.

The fullscreen pair - the same frames as they come, then asking compositing to
step aside - is read in power, not frames, because the saving is the
compositor's work and not the application's.

No workload repeats another: the scripted person is split into `windows`
(maximize, raise, fullscreen, minimize - states only), `scroll`, `resize` and
`dnd`.

Every load says where its windows landed, and the moving tests check they
really moved. A test the window manager cannot run is flagged "not done", and
one that tries and fails is flagged "failed" - neither is filled with the small
number a window manager doing nothing produces.

## Tiling window managers

A tiling window manager chooses the size of the windows it manages, so a test
may draw a window far bigger or smaller than `wmbench` asks for, which can
lead to incompatible results.

Although some tests are not affected because they open their windows outside
the window manager, others are, such as: `windows`, `dnd`, `render`,
`fullscreen` and `uncapped`. Every test compares the size it got with the size
it asked for, and the report flags the ones that differ.

## validate.sh

Nine tests, one visual defect each. Each logs the geometry it asked for
against what it got and photographs the window's own content. The screen has
to be idle, or the tests might fail.

Each answers **passed**, **failed** (the window manager is at fault), **could
not run** (no display, no way to photograph the screen) or **not done** (the
window manager does not do the thing tested).

Wayland needs a screenshot tool: `grim`, or `gnome-screenshot` or `spectacle`
together with `ffmpeg` to convert their PNG. Without one the pixel tests are
skipped. `shape_check` and `leftover_check` are always "not done" there:
Wayland has no window shapes, and no protocol tells one window's place from
another's.

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

The stability pass runs everything at once, with `motion_check`'s pattern
scrolling in the middle: the same question asked of a busy compositor. Windows
open one at a time so every session composites the same scene. On Wayland the
stacking order cannot be read back, so it goes unverified.

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
