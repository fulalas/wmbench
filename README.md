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

Every binary carries an X11 backend and a native Wayland one and picks at
startup: with `WAYLAND_DISPLAY` set it speaks Wayland directly, otherwise
Xlib. `BENCH_BACKEND=x11` on a Wayland session forces the X11 backend, which
then runs through XWayland - the same load twice is how XWayland's own cost is
measured. `BENCH_OUTPUT=<name>` picks the monitor on Wayland; the first one
otherwise.

Wayland denies clients some of what X11 grants, so what runs natively depends
on the protocols the compositor speaks, probed at startup:

| protocol | gives | spoken by |
| --- | --- | --- |
| wlr-layer-shell | putting a window at a screen coordinate | KWin, cosmic-comp, labwc, sway, hyprland - not mutter |
| xx-zones (experimental) | placing and moving managed toplevels, positions reported back | KWin so far |
| xdg-activation | raising and focusing own windows | everyone |
| wp-alpha-modifier | whole-window opacity | KWin, sway, hyprland, labwc - not mutter |
| xdg-shell | maximize, minimize, fullscreen, popups | everyone |
| xdg-decoration | a compositor-drawn frame, as every X11 window gets | most - not mutter |
| wlr-foreign-toplevel | what a taskbar knows: other windows, unminimise | wlroots compositors - not KWin, not mutter |

Where a protocol is missing the tests that need it fire the same refusal
channels as always - `PLACE-IGNORED`, `MOVE-REFUSED`, exit 3 - and the row
reads "not done" or "not available" instead of holding a number for work that
never happened. On GNOME that is every test built out of placed windows.

Some Wayland rows measure a differently-shaped ask, said here once:

- **move** repositions a layer surface by its own margins: the surface really
  travels and the compositor really composites it, but no window manager is
  moving a frame. The probe proves the travel with a screenshot before the
  measurement and once after it, never inside it; without a screenshot tool
  the row says its moves are unproven.
- **windows** walks the same states through xdg-shell and verifies them by the
  compositor's own configure events. Under zone placement the walks are real
  moves, positions confirmed by the compositor's own events; elsewhere
  nothing a client says moves a managed toplevel, so the twelve-step walk to
  each screen edge is carried by the size instead - the same number of
  composited steps on the way to the same tile. The restore from minimize
  goes the way a taskbar does it where the compositor lists foreign
  toplevels, and through an xdg-activation token elsewhere - a compositor
  that refuses both ends the row with exit 3.
- **zone placement** answers moves the way the X server does - the compositor
  reports every position back - so those rows need no screenshot proof. What
  a zone will not say is where it sits on the screen, deliberately, so the
  pixel checks never aim a capture through it; they use layer-shell or say
  "not available".
- **transbench** is asymmetric by design: X11 sets `_NET_WM_WINDOW_OPACITY`
  blind and measures whatever the compositor makes of it, while on Wayland a
  compositor without alpha-modifier is asked, answers no, and the row is "not
  done" rather than a number for blending that never happened.
- **fullscreen asked** exists only on X11: nothing on Wayland asks a
  compositor to step aside, so the row answers "not done" there.

Fractional scaling breaks the 1:1 map between a window's pixels and a
screenshot's, so under it every pixel check honestly answers "could not look";
integer scales are handled. The `BENCH_CAPTURE_CMD` screenshot command must
write the chosen output alone, which `grim` does on a single-monitor desktop.

## benchmark.sh

Every workload does a **fixed amount of work**, so two sessions compare
directly and the time it took is a result rather than a setting.

The exception is the frame rate, which is deliberately flat out and reports
only frames a second. It runs once, windowed, and last.

What leaving a window out of compositing is worth is measured separately: the
same fixed number of frames in a fullscreen window, twice - as it comes, and
asking compositing to step aside with `_NET_WM_BYPASS_COMPOSITOR`. That pair
is read in power, not frames, because the saving is the compositor's work and
not the application's.

No workload repeats work another one does: the scripted person is split into
`windows` (maximize, minimize, snap, raise, fullscreen) and `scroll`, next to
the `resize` and `dnd` tests.

Every load says where its windows actually landed, and the tests built out of
moving windows check that the windows really moved. Where a compositor
refuses - cosmic-comp will not reposition an X11 window it manages, mutter
will not place a Wayland one anywhere at all - the test is left empty and
named at the end rather than filled with the small number a compositor doing
nothing produces.

The CPU column is the window manager's own process. On X11 part of the
compositing happens inside the X server and is not in it - but the server also
does every program's drawing, which on Wayland happens inside the programs
themselves, so adding it would tilt the comparison the other way rather than
fix it. Read X11 numbers against X11 ones.

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

On Wayland a screenshot tool is required: `grim`, or `gnome-screenshot` or
`spectacle` together with `ffmpeg`, which converts their PNG. Without one the
pixel tests are skipped. Two "not available" verdicts are inherent to Wayland
rather than any compositor's fault: `shape_check`, because there is no shape
concept there (the defect it hunts lives in per-pixel alpha, argbbench's
ground), and `leftover_check`, because no protocol tells one window's place
from another's, so nothing can be masked out of the comparison. The rest of
the checks run natively on layer-shell compositors and answer "not available"
on the ones without it.

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

The stability pass at the end runs everything at once, all of it started and
finished together, with `motion_check`'s pattern scrolling in the middle of it:
the same question asked of a compositor that is busy. The windows are opened
one at a time, each waited for before the next, so they stack in the order they
were opened and every session composites the same scene. On X11 the waiting and
the final order are asked of the server; on Wayland each tool prints a
`WINDOW-UP` marker after its first commit and the scripts wait on that, since
nothing there can read the order back - the report says so rather than
claiming an order nobody verified.

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
