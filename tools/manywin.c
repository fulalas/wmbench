/*
 * A desktop with a lot of windows on it, which nothing here has covered. The
 * eight-window stress had eight windows all drawing flat out, measuring
 * throughput; this is the other shape of the same question: many windows
 * sitting there doing nothing while one small thing animates, which is what a
 * real desktop looks like.
 *
 * It is what decides whether skipping the blended pass for windows whose
 * shadow is nowhere near the damage is worth its four rectangle tests.
 *
 *   manywin <count> [seconds]
 *
 * Opens <count> ordinary managed windows with content, spread over the screen,
 * and holds them there. With no seconds given, or zero, it holds them until it
 * is killed: the windows are scenery, not work, so there is nothing to count
 * here and nothing that should end a measurement. Whoever started it decides
 * when the screen is no longer needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "stage.h"
#include "place.h"

#define WINW 420
#define WINH 320

int main (int argc, char **argv)
{
    Display *d;
    Window *wins;
    Pixmap pat[6];
    GC gc;
    Atom wtype, normal;
    int count = (argc > 1) ? atoi (argv[1]) : 20;
    double seconds = (argc > 2) ? atof (argv[2]) : 0.0;
    int scr, i, j, cols, sw, sh, sx, sy;
    unsigned long colours[6] = {
        0x904040, 0x409040, 0x404090, 0x909040, 0x904090, 0x409090
    };

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    bench_stage (d, 60, &sx, &sy, &sw, &sh);
    cols = sw / (WINW + 40);
    if (cols < 1)
    {
        cols = 1;
    }

    if (count < 1)
    {
        fprintf (stderr, "count must be at least 1\n");

        return 2;
    }
    wins = calloc (count, sizeof (Window));
    if (wins == NULL)
    {
        fprintf (stderr, "out of memory\n");

        return 2;
    }
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);

    /*
     * The content is the windows' background rather than something painted
     * once after mapping. Nothing here listens for Expose, and these windows
     * are scenery other benchmarks move over - stress_start() walks two
     * windows across them - so a strip that is uncovered gets filled from the
     * background: painted once it would come back black and stay black,
     * wherever nothing composites and keeps the contents for us. The pattern
     * only depends on i % 6, so six of them cover any number of windows.
     */
    gc = XCreateGC (d, RootWindow (d, scr), 0, NULL);
    for (i = 0; i < 6; i++)
    {
        pat[i] = XCreatePixmap (d, RootWindow (d, scr), WINW, WINH,
                                (unsigned) DefaultDepth (d, scr));
        XSetForeground (d, gc, BlackPixel (d, scr));
        XFillRectangle (d, pat[i], gc, 0, 0, WINW, WINH);
        for (j = 0; j < 24; j++)
        {
            XSetForeground (d, gc, colours[(i + j) % 6]);
            XFillRectangle (d, pat[i], gc, (j * 31) % (WINW - 70),
                            (j * 43) % (WINH - 50), 70, 50);
        }
    }

    for (i = 0; i < count; i++)
    {
        XSizeHints hints;
        /* However many fit; the rest start again from the top, offset */
        int rows = sh / (WINH + 60);
        int x, y;

        if (rows < 1)
        {
            rows = 1;
        }
        x = sx + (i % cols) * (WINW + 40);
        y = sy + ((i / cols) % rows) * (WINH + 60);
        x += 25 * (i / (cols * rows));

        wins[i] = XCreateSimpleWindow (d, RootWindow (d, scr), x, y,
                                       WINW, WINH, 0, BlackPixel (d, scr),
                                       BlackPixel (d, scr));
        XSetWindowBackgroundPixmap (d, wins[i], pat[i % 6]);
        /*
         * Cleared first: only the flags below are set, but the whole struct
         * still travels to the window manager, and whatever the stack held
         * reads as a minimum size, a size step or an aspect ratio.
         */
        memset (&hints, 0, sizeof hints);
        hints.flags = USPosition | USSize | PPosition | PSize;
        hints.x = x; hints.y = y;
        hints.width = WINW; hints.height = WINH;
        XSetWMNormalHints (d, wins[i], &hints);
        XChangeProperty (d, wins[i], wtype, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *) &normal, 1);
        XStoreName (d, wins[i], "manywin");
        XMapWindow (d, wins[i]);
    }
    XSync (d, False);
    sleep (3);
    bench_placed (d, wins[0], sx, sy, "manywin");

    printf ("READY %d windows\n", count);
    fflush (stdout);
    if (seconds > 0.0)
    {
        usleep ((useconds_t) (seconds * 1e6));
    }
    else
    {
        for (;;)
        {
            pause ();
        }
    }

    for (i = 0; i < count; i++)
    {
        XDestroyWindow (d, wins[i]);
    }
    for (i = 0; i < 6; i++)
    {
        XFreePixmap (d, pat[i]);
    }
    XCloseDisplay (d);
    free (wins);

    return 0;
}
