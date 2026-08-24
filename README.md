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

## benchmark.sh

Every workload does a **fixed amount of work**, so two sessions compare
directly and the time it took is a result rather than a setting.

The exception is the frame rate, which is deliberately flat out and reports
only frames a second. It runs three times: windowed, fullscreen, and
fullscreen asking compositing to step aside with `_NET_WM_BYPASS_COMPOSITOR`.

## validate.sh

Nine tests, each hunting one visual defect. `validate.sh` runs them all, then
logs the geometry it asked for against what it got and photographs the window's
own content, so a compositor that quietly ignores a move is caught.

The tests need an idle screen, otherwise they might fail.

What happens after the framebuffer is beyond every check here: corruption
produced in the display pipeline, by colour compression for instance, is on the
panel but in no capture, so a clean verdict does not cover it.

Each test answers with one of four statuses, which is what the report prints:
**passed**, **failed** (the compositor is at fault), **could not run** (no
display, no way to photograph the screen) and **not available** (the compositor
does not do the thing being tested, so nothing was proved either way).

On Wayland a screenshot tool is required (`grim`, `gnome-screenshot` or
`spectacle`), otherwise the pixel tests are skipped.

| test | the defect it catches |
| --- | --- |
| `motion_check` | tearing: a capture taken while a pattern scrolls that is not one single frame |
| `stale_check` | staleness: leftovers after scrolling stops, and a window nobody draws to while another is hammered |
| `pop_check` | what a menu covered not coming back when it closes |
| `suspend_check` | the screen not coming back after compositing suspends for a fullscreen window |
| `shape_check` | a non-rectangular window painting its undefined corners over what is behind it |
| `resize_check` | a frame drawn from a window pixmap that was not ready yet, during continuous resizing |
| `offscreen_check` | a window hanging off the left or top edge showing the wrong part of itself |
| `iconify_check` | a window not coming back correctly after being minimised |
| `leftover_check` | garbage left on the desktop by the load, by comparing the idle screen before it with the same screen after |

The stability pass at the end runs everything at once, all of it started and
finished together, with `motion_check`'s pattern scrolling in the middle of it:
the same question asked of a compositor that is busy.

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
