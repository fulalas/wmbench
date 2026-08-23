# wmbench

Benchmarks and artifact checks for window managers. Works on any window
manager, X11 or Wayland: it measures the session as it is, so two desktops can
be put side by side.

    make                    build everything
    ./benchmark.sh          measure the session, print a table
    ./validate.sh           look for visual defects, print a verdict
    ./compare_results.sh    put the saved runs side by side

Both entry points build what they need, work out which compositor is running,
and save their table under `results/`. With no power sensor everything still
runs and the power column reads `-`. `argbbench`, `multiscene` and `videobench`
are extra workloads to run by hand.

## benchmark.sh

Every test does a **fixed amount of work**, so two sessions compare directly
and the time it took is a result rather than a setting.

The exception is the frame-rate test, which is deliberately flat out and
reports only frames a second. It runs three times: windowed, fullscreen, and
fullscreen asking compositing to step aside with `_NET_WM_BYPASS_COMPOSITOR`.

## validate.sh and the checks

`validate.sh` runs the checks, logs the geometry it asked for against what it
got, photographs the window's own content, and films the pass where the
session can be recorded. Two runs are compared with
`./compare_runs.sh <A> <B>`: geometry with an allowance for decorations,
pixels exactly. That is how a compositor that quietly ignores a move is
caught.

    make check      run the eight checks against the running compositor

Restart the compositor first - the checks test what is running, not what was
just built. They also need an idle screen: anything else animating on it will
flip them. On Wayland they need a screenshot tool (`grim`, `gnome-screenshot`
or `spectacle`), since only the compositor can hand the screen out; without
one the pixel checks are skipped and say so.

| check | what it catches |
| --- | --- |
| `motion_check` | tearing: a capture taken while a pattern scrolls that is not one single frame |
| `stale_check` | staleness: leftovers after scrolling stops, and a window nobody draws to while another is hammered |
| `pop_check` | what a menu covered not coming back when it closes |
| `suspend_check` | the screen not coming back after compositing suspends for a fullscreen window |
| `shape_check` | a non-rectangular window painting its undefined corners over what is behind it |
| `resize_check` | a frame drawn from a window pixmap that was not ready yet, during continuous resizing |
| `offscreen_check` | a window hanging off the left or top edge showing the wrong part of itself |
| `iconify_check` | a window not coming back correctly after being minimised |

Each has been shown to fail on a deliberately broken build; a check that has
never failed proves nothing. `shape_check` needs XShape. `stale_check` is the
sensitive one. `motion_check` samples every fourth row, so a stale patch can
slip between its samples. `resize_check` takes a `managed` argument to let the
window manager frame the window; that mode false-positives about 1 time in 800
by capturing mid-reframe, so read it alongside the plain mode.

## The power metric

Two sensors, added together when both are needed:

| what you have | what the watts include |
| --- | --- |
| AMD integrated GPU | CPU and GPU together, in one number |
| AMD discrete GPU | the GPU, plus the CPU from its own sensor |
| Intel integrated GPU | CPU and GPU together, in one number |
| NVIDIA discrete GPU, NVIDIA driver | the GPU, asked of `nvidia-smi`, plus the CPU |
| NVIDIA discrete GPU, open driver | the CPU only - the GPU will not report its power |

Which sensors were used is printed above the table, so a figure that covers
less than the whole machine says so instead of passing for one. The CPU sensor
is root-only on most kernels, one `chmod` away:

    sudo chmod a+r /sys/class/powercap/intel-rapl:0/energy_uj

`nvidia-smi` is asked once for a whole run, not once per sample: starting it
ten times a second would land in the CPU figures being measured.

Sensors are found by driver name, never by index: hwmon numbering is not stable
across boots, and nvme drives and wireless cards expose `power1_average` too.
