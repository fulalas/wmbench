/*
 * A client-side-decorated window, which is what every GTK3 and GTK4
 * application is and what no other benchmark here measures: 32-bit ARGB
 * visual, full window opacity, and _NET_WM_OPAQUE_REGION declaring everything
 * except a margin for its own rounded corners and shadow.
 *
 * The compositor cannot treat such a window as opaque, so by default it blends
 * the whole of it every frame, which reads the destination as well as the
 * texture. Only the margin actually needs that.
 *
 *   argbbench <seconds> [steps per second] [margin]
 *
 * BENCH_TASKS=N does N steps instead of running for the seconds given.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include "gate.h"
#include "now.h"

#define WINW 1600
#define WINH 1000

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

int main (int argc, char **argv)
{
    Display *d;
    Window win, root;
    GC gc;
    XVisualInfo tmpl, *vi;
    XSetWindowAttributes swa;
    XSizeHints hints;
    Atom wtype, normal, opaque_atom;
    double seconds = (argc > 1) ? atof (argv[1]) : 12.0;
    double rate = (argc > 2) ? atof (argv[2]) : 120.0;

    if (rate <= 0.0)
    {
        rate = 120.0;
    }
    int margin = (argc > 3) ? atoi (argv[3]) : 40;
    int scr, nvi, i, j, steps = 0, warm;
    long tasks, done = 0;
    double start, mstart;
    long opaque[4];
    unsigned long colours[6] = {
        0xffc04040, 0xff40c040, 0xff4040c0, 0xffc0c040, 0xffc040c0, 0xff40c0c0
    };

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    /* A 32-bit visual, the way a toolkit doing its own decorations asks */
    tmpl.screen = scr;
    tmpl.depth = 32;
    tmpl.class = TrueColor;
    vi = XGetVisualInfo (d, VisualScreenMask | VisualDepthMask | VisualClassMask,
                         &tmpl, &nvi);
    if (vi == NULL || nvi == 0)
    {
        fprintf (stderr, "no 32-bit visual\n");

        return 2;
    }

    swa.colormap = XCreateColormap (d, root, vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.background_pixel = 0;
    win = XCreateWindow (d, root, 120, 120, WINW, WINH, 0, 32, InputOutput,
                         vi->visual, CWColormap | CWBorderPixel | CWBackPixel,
                         &swa);

    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = 120; hints.y = 120; hints.width = WINW; hints.height = WINH;
    XSetWMNormalHints (d, win, &hints);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, win, "argbbench");

    /*
     * Everything but the margin is opaque, which is exactly what GTK says.
     * The rectangle is relative to the client window.
     */
    opaque_atom = XInternAtom (d, "_NET_WM_OPAQUE_REGION", False);
    opaque[0] = margin;
    opaque[1] = margin;
    opaque[2] = WINW - 2 * margin;
    opaque[3] = WINH - 2 * margin;
    XChangeProperty (d, win, opaque_atom, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *) opaque, 4);

    XMapWindow (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (3);

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 10 : 0;
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;

        /* Redraw inside the opaque part, the way an application's content does */
        for (j = 0; j < 4; j++)
        {
            XSetForeground (d, gc, colours[(i + j) % 6]);
            XFillRectangle (d, win, gc,
                            margin + ((i * 37 + j * 211) % (WINW - 2 * margin - 400)),
                            margin + ((i * 53 + j * 97) % (WINH - 2 * margin - 300)),
                            400, 300);
        }
        XSync (d, False);
        steps++;

        if (tasks > 0)
        {
            if (i + 1 == warm)
            {
                mark ("MEASURE-START");
                mstart = bench_now ();
                /*
                 * The gate in mark() can have held this a long time, and the
                 * schedule counts from start: left alone it would run flat out
                 * to catch up, which is not the fixed rate this load is for.
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
        printf ("AVERAGE %.1f steps/s over %.0f s\n",
                steps / (bench_now () - start), seconds);
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);
    XFree (vi);

    return 0;
}
