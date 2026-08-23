# wmbench

Benchmarks and visual defect tests for window managers. Works on any window
manager, X11 or Wayland: it measures the session as it is, so two desktops can
be put side by side.

    make                    build everything
    ./benchmark.sh          measure the session, print a table
    ./validate.sh           look for visual defects, print a verdict
    ./compare_results.sh    put the saved runs side by side

Both entry points build what they need, work out which compositor is running,
and save their table under `results/`. With no power sensor everything still
runs and the power column reads `-`.

## benchmark.sh

Every test does a **fixed amount of work**, so two sessions compare directly
and the time it took is a result rather than a setting.

The exception is the frame-rate test, which is deliberately flat out and
reports only frames a second. It runs three times: windowed, fullscreen, and
fullscreen asking compositing to step aside with `_NET_WM_BYPASS_COMPOSITOR`.

## validate.sh

Eight tests, each hunting one visual defect. `validate.sh` runs them all, logs
the geometry it asked for against what it got, photographs the window's own
content, and films the pass where the session can be recorded, so a compositor
that quietly ignores a move is caught.

The tests look at whatever compositor is running and never touch it. They do
need an idle screen: anything else animating on it will flip them.

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

Each has been shown to fail on a deliberately broken build; a test that has
never failed proves nothing. `shape_check` needs XShape. `stale_check` is the
sensitive one. `motion_check` samples every fourth row, so a stale patch can
slip between its samples. `resize_check` takes a `managed` argument to let the
window manager frame the window; that mode false-positives about 1 time in 800
by capturing mid-reframe, so read it alongside the plain mode.

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
