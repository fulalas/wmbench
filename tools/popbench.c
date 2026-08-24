/*
 * Windows appearing and disappearing: menus, tooltips, notifications. The
 * one case where the per-window setup cost dominates rather than the
 * per-frame drawing. The GL renderer has to
 * build a GLX pixmap, a texture and a binding for every window that appears,
 * and the first bind of a new pixmap waits for the server with a round trip;
 * XRender only has to make a picture.
 *
 * A background window gives the compositor something to redraw underneath.
 * Override-redirect popups, the way real menus are, are mapped and unmapped in
 * turn at a fixed rate so every renderer is given identical work.
 *
 *   popbench <seconds> [cycles per second] [popups] [width] [height]
 *
 * A small height matters: the GL renderer draws shadows from one shared
 * procedural profile only for windows at least twice the blur radius, and falls
 * back to building a gaussian on the CPU and uploading it for anything
 * smaller. Tooltips are short and wide, so they take the fallback.
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
#include "stage.h"

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

#define BGW  1600
#define BGH  1000
static int popw = 340;
static int poph = 440;

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
    Window bg, *pop;
    GC gc;
    XSetWindowAttributes swa;
    XSizeHints hints;
    Atom wtype, normal;
    double seconds = (argc > 1) ? atof (argv[1]) : 12.0;
    double rate = (argc > 2) ? atof (argv[2]) : 20.0;

    if (rate <= 0.0)
    {
        rate = 20.0;
    }
    int npop = (argc > 3) ? atoi (argv[3]) : 6;
    int scr, i, j, cycles = 0;
    int bgw, bgh, bgx, bgy, maxx;
    long tasks, warm, done = 0;
    double mstart;

    if (argc > 4) popw = atoi (argv[4]);
    if (argc > 5) poph = atoi (argv[5]);
    double start;
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

    bgw = BGW; bgh = BGH;
    bench_stage (d, 80, &bgx, &bgy, &bgw, &bgh);
    bgw = MIN (BGW, bgw); bgh = MIN (BGH, bgh);
    /* How far along the popups may march before starting again */
    maxx = bgw - 200 - popw;
    if (maxx < 1)
    {
        maxx = 1;
    }
    bg = XCreateSimpleWindow (d, RootWindow (d, scr), bgx, bgy, bgw, bgh, 0,
                              BlackPixel (d, scr), BlackPixel (d, scr));
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = bgx; hints.y = bgy; hints.width = bgw; hints.height = bgh;
    XSetWMNormalHints (d, bg, &hints);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, bg, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, bg, "popbench background");
    XMapWindow (d, bg);
    gc = XCreateGC (d, bg, 0, NULL);
    XSync (d, False);
    sleep (3);

    for (i = 0; i < 300; i++)
    {
        XSetForeground (d, gc, colours[i % 6]);
        XFillRectangle (d, bg, gc, (i * 41) % (bgw - 80), (i * 67) % (bgh - 60),
                        80, 60);
    }
    XSync (d, False);

    /* Override redirect, the way a menu really is: no frame, no manager */
    if (npop < 1)
    {
        fprintf (stderr, "popups must be at least 1\n");

        return 2;
    }
    pop = calloc (npop, sizeof (Window));
    if (pop == NULL)
    {
        fprintf (stderr, "out of memory\n");

        return 2;
    }
    swa.override_redirect = True;
    for (i = 0; i < npop; i++)
    {
        /* Inside the stage, and inside the background window with it */
        pop[i] = XCreateWindow (d, RootWindow (d, scr),
                                bgx + 100 + (i * (popw + 30)) % maxx,
                                bgy + 100 + (i % 3) * (poph + 40),
                                popw, poph, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWOverrideRedirect, &swa);
    }
    XSync (d, False);

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 10 : 0;
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;
        Window w = pop[i % npop];

        XMapRaised (d, w);
        for (j = 0; j < 10; j++)
        {
            XSetForeground (d, gc, colours[(i + j) % 6]);
            XFillRectangle (d, w, gc, 10, 10 + j * 42, popw - 20, 36);
        }
        XSync (d, False);
        usleep (12000);
        XUnmapWindow (d, w);
        XSync (d, False);
        cycles++;

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
                cycles = 0;
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
        printf ("AVERAGE %.1f cycles/s over %.1f s, %ld cycles\n",
                cycles / (bench_now () - mstart), bench_now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.1f cycles/s over %.0f s\n",
                cycles / (bench_now () - start), seconds);
    }

    for (i = 0; i < npop; i++)
    {
        XDestroyWindow (d, pop[i]);
    }
    XDestroyWindow (d, bg);
    XCloseDisplay (d);
    free (pop);

    return 0;
}
