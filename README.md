# wmbench

Benchmarks and artifact checks for window managers. Everything here works on
any window manager, X11 or Wayland: the session is measured as it is, so two
desktops can be put side by side.

    make              build everything
    ./validate.sh     run the checks and print a verdict
    ./benchmark.sh    measure the session and print a table

| folder | holds |
| --- | --- |
| `tools/` | the workload programs the benchmarks drive |
| `checks/` | the eight artifact checks |
| `lib/` | code shared by the above |

## Any window manager: the two entry points

    ./validate.sh     build, detect the compositor, run the artifact checks
                      and a stability pass, print a verdict
    ./benchmark.sh    build, detect the compositor, measure it, print a table

Both work from scratch on whatever session they are started in, and are how
different desktops are compared: run them in each session, then

    ./compare_results.sh

puts every result in `results/` side by side - one column per desktop, runs of
the same desktop averaged - and writes the notes that say what the numbers can
and cannot mean. Without a power sensor everything still runs and the power column
just reads `-`.

Every benchmark test does a **fixed amount of work** - so many moves, so many
menus, so many frames - however long that takes, so the power and processor
figures of two sessions compare directly and the time it took is a result
rather than a setting. Nothing is ever cut short on the clock. The single
exception is the frame-rate test, which is deliberately flat out and reports
only frames a second. It runs last and three times: windowed, fullscreen, and
fullscreen with `_NET_WM_BYPASS_COMPOSITOR` set, which is how a player asks
compositing to step aside. A fullscreen window draws far more pixels than the
windowed one, so those two are not comparable with each other - the pair of
fullscreen figures is. Everything printed is also saved to
`results/<script>-<wm>-<session>-<host>-<time>.txt`, so the runs from
different desktops can be collected and compared later.

Both include `usagebench`, a scripted person: windows are maximized and
restored, minimized and restored, walked to the screen sides and snapped to
half the screen, walked to the four corners clockwise and counter-clockwise
and snapped to a quarter, raised over each other like alt-tab, and sent
fullscreen and back. It is also resized by every handle - the bottom-right
corner, each edge, and the top-left corner, which moves the window as it
grows - always staying on screen, and icons are dragged one by one from one
window into another and then carried back all at once, each drag hanging from
a small translucent window the way a real one does. Scrolling gets two tests
of its own in a window holding
a 200-line text document with a working scroll bar: the chevron buttons, and
the thumb dragged the whole way down and back. Both scroll smoothly, a few
pixels a frame at 60 a second, which is what the compositor has to keep up
with. It is plain X11 and EWMH with no input
injection, so every session - any WM, X11 or Wayland - does exactly the same
work and the numbers compare.

`validate.sh` also records checkpoints: after each snap and scroll it logs the
geometry that was asked for against what the WM did, and photographs the
window's own content (never the desktop around it). The whole pass is filmed
too when the session can be recorded - ffmpeg on X11, `wf-recorder` on
labwc/COSMIC/sway, GNOME through its shell's screencast (needs PipeWire
running) - so a WM that handles a move differently is not just flagged but
visible. Two sessions' folders are
then compared with `./compare_runs.sh <A> <B>`: geometry with a 100 px
allowance for decorations, pixels exactly. That is how a WM that quietly
ignores a move or resize is caught. Detection covers GNOME (mutter), KDE Plasma (KWin), Xfce (xfwm4 on
X11, labwc on Wayland), LXQt/LXDE (openbox on X11, labwc on Wayland),
Cinnamon (muffin), COSMIC (cosmic-comp) and MATE (marco), with a process scan
and the X11 WM name as fallbacks.

On Wayland the checks cannot photograph the screen through X11, so they use a
screenshot tool, because only the compositor can hand the screen out:
`grim` (labwc, COSMIC, sway), `gnome-screenshot` (GNOME) or `spectacle`
(KDE), with ffmpeg converting.
Captures are slow that way, so the moving checks run shortened; with no tool
at all the pixel checks are skipped and said so. For GNOME and Cinnamon the
compositor lives inside the shell process, so the CPU column includes the
whole shell, and the table says that too.

    make            build the benchmarks and the checks
    make check      run the eight artifact checks

`make check` tests the compositor that is *running*, not the one just built.
Restart it first, or the checks will report on the previous binary.

## The artifact checks

Any change to the presentation path has to pass all eight. Each has been shown
to fail on a deliberately broken build; a check that has never failed proves
nothing.

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

`shape_check` needs XShape. `resize_check` takes a `managed` argument to let the window manager frame
the window; that mode has a false-positive rate of about 1 in 800 from capturing
while the manager is still reframing, and its teeth are unproven, so read it
alongside the override-redirect mode rather than on its own. `stale_check` is the
sensitive one. `motion_check` samples every fourth row and
a stale patch can slip between its samples, so passing it alone means less than
it looks.

## Two rules, learned the hard way

Both of these produced a confident wrong answer in this project before, and
getting either wrong produced confident wrong answers before.

**Alternate the arms.** Whichever variant runs first in a round comes out cooler
and wins, whatever it is — demonstrated over four rounds where the first arm won
every time, twice for each binary. A comparison that does not alternate is
worthless however tight its samples look. One `glBlitFramebuffer` experiment
looked 9.4% faster on three consistent samples an arm and was worth exactly
nothing once alternated.

**Compare within one condition.** A per-paint processor or GPU figure is not
comparable between an idle desktop and a loaded one, or between a capped and an
uncapped run: the clocks differ. The same code measures 0.417 ms a paint at a
capped 60 fps and 0.379 uncapped. Three separate mistakes in this file came from
mixing conditions.

## The metric

Package power, from the hwmon exposing `power1_average`. On an AMD APU that is
labelled PPT and covers the processor cores as well as the graphics, which is
what makes it the only metric here that can see both our process and the X
server, where XRender does its work.

`common.sh` finds it by driver name and refuses to guess: never hardcode an
hwmon index, the numbering is not stable across boots, and never take whatever
happens to expose `power1_average`, because nvme drives and wireless cards do
too and a disk's power draw looks just as plausible in the output.

**On a discrete card this metric does not work.** The same file is board power
and excludes the processor, so the compositor's processor-side cost is invisible
in it. Reproducing the power figures needs an APU, or RAPL. The frame-rate
figures have no such dependency.

Whole-machine CPU time is retired: on this hardware the compositor costs under
1% of one core and the idle background moves by more than that between runs.
