/*
 * Translucency. Nothing else here asks the compositor to blend a whole
 * managed window uniformly: argbbench hands it per-pixel alpha and popbench
 * hands it shadows. Yet a translucent terminal over a busy window is one of
 * the commonest things on a real desktop, and it is the case where the
 * compositor cannot just copy: it has to read what is underneath and blend.
 *
 * A detailed opaque window sits underneath. A second window on top carries
 * _NET_WM_WINDOW_OPACITY and redraws at a fixed rate, so every step forces the
 * compositor to repaint the background there and blend the top window over it.
 *
 *   transbench <seconds> [opacity 0..1] [steps per second]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "polite.h"
#include "gate.h"
#include "now.h"
#include "stage.h"
#include "place.h"

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

#define BGW 1700
#define BGH 1050
#define FGW 900
#define FGH 600

/*
 * Fixed work: BENCH_TASKS=N does exactly N tasks, however long that takes, so
 * every session performs the same amount of work and the numbers compare. The
 * measured part is bracketed by the two marks, after an unmeasured warm-up.
 */
static long bench_tasks (void)
{
    const char *e = getenv ("BENCH_TASKS");

    return (e != NULL && *e != '\0') ? atol (e) : 0;
}

static void mark (const char *s)
{
    if (strcmp (s, "MEASURE-START") == 0)
    {
        /* In a mix of programs, wait until the whole load is up. See gate.c */
        bench_wait_go ();
    }
    printf ("%s\n", s);
    fflush (stdout);
}

static Window make_window (Display *d, int scr, int x, int y, int w, int h,
                           const char *name)
{
    Window win;
    XSizeHints hints;
    Atom wtype, normal;

    win = XCreateSimpleWindow (d, RootWindow (d, scr), x, y, w, h, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    XStoreName (d, win, name);

    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = x; hints.y = y;
    hints.width = w; hints.height = h;
    XSetWMNormalHints (d, win, &hints);

    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);

    return win;
}

int main (int argc, char **argv)
{
    Display *d;
    Window bg, fg;
    GC gc;
    Atom opacity_atom;
    unsigned long opacity;
    double seconds = (argc > 1) ? atof (argv[1]) : 12.0;
    double alpha = (argc > 2) ? atof (argv[2]) : 0.75;
    double rate = (argc > 3) ? atof (argv[3]) : 120.0;

    /* A 32-bit CARDINAL: out of range it would wrap rather than saturate */
    if (alpha < 0.0)
    {
        alpha = 0.0;
    }
    if (alpha > 1.0)
    {
        alpha = 1.0;
    }
    if (rate <= 0.0)
    {
        rate = 120.0;
    }
    int scr, i, steps = 0;
    int bgw, bgh, fgw, fgh, sx, sy;
    long tasks, warm, done = 0;
    double start, mstart;
    unsigned long colours[6] = {
        0xc04040, 0x40c040, 0x4040c0, 0xc0c040, 0xc040c0, 0x40c0c0
    };

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);

    bench_stage (d, 60, &sx, &sy, &bgw, &bgh);
    bgw = MIN (BGW, bgw); bgh = MIN (BGH, bgh);
    fgw = MIN (FGW, bgw - 220); fgh = MIN (FGH, bgh - 220);
    bg = make_window (d, scr, sx, sy, bgw, bgh, "transbench background");
    fg = make_window (d, scr, sx + 220, sy + 220, fgw, fgh,
                      "transbench translucent");

    /* The window manager reads this and tells the compositor to blend */
    opacity_atom = XInternAtom (d, "_NET_WM_WINDOW_OPACITY", False);
    opacity = (unsigned long) (alpha * 0xffffffffUL);
    XChangeProperty (d, fg, opacity_atom, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *) &opacity, 1);

    XMapWindow (d, bg);
    XMapWindow (d, fg);
    gc = XCreateGC (d, bg, 0, NULL);
    XSync (d, False);
    sleep (3);
    XRaiseWindow (d, fg);
    /* Politely, or GNOME posts a notification instead of raising */
    polite_activate (d, RootWindow (d, scr), fg);
    XSync (d, False);
    sleep (1);
    bench_placed (d, bg, sx, sy, "transbench background");
    bench_placed (d, fg, sx + 220, sy + 220, "transbench translucent");

    /* Detail underneath, so the blend has something to read */
    for (i = 0; i < 400; i++)
    {
        XSetForeground (d, gc, colours[i % 6]);
        XFillRectangle (d, bg, gc, (i * 37) % (bgw - 90), (i * 61) % (bgh - 70),
                        90, 70);
    }
    XSync (d, False);

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 60 : 0;
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;

        XSetForeground (d, gc, colours[i % 6]);
        XFillRectangle (d, fg, gc, (i * 23) % (fgw - 200),
                        (i * 29) % (fgh - 150), 200, 150);
        XSync (d, False);
        steps++;

        if (tasks > 0)
        {
            if (i + 1 == warm)
            {
                mark ("MEASURE-START");
                mstart = bench_now ();
                /*
                 * The gate in mark() can have held this for a long time, and
                 * the schedule below counts from start: left alone it would
                 * now run flat out to catch up, which is not the fixed rate
                 * this load is supposed to make. Put it back on the clock.
                 */
                start = mstart - (double) (i + 1) / rate;
                steps = 0;
            }
            else if (i + 1 > warm)
            {
                done++;
            }
            if (done >= tasks)
            {
                break;
            }
        }
        else if (bench_now () - start >= seconds)
        {
            break;
        }

        while (bench_now () < due)
        {
            usleep (200);
        }
    }

    if (tasks > 0)
    {
        mark ("MEASURE-END");
        printf ("AVERAGE %.1f steps/s over %.1f s, %ld steps\n",
                steps / (bench_now () - mstart), bench_now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.1f steps/s over %.0f s\n", steps / (bench_now () - start),
                seconds);
    }

    XDestroyWindow (d, fg);
    XDestroyWindow (d, bg);
    XCloseDisplay (d);

    return 0;
}
