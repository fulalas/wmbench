/*
 * Captures the screen WHILE a known pattern scrolls, so corruption that only
 * exists during motion is seen. Every capture must show one single offset of
 * the pattern; a capture mixing two offsets is tearing or stale pixels.
 *
 * Replaces motion_check.py, whose python-xlib and PIL are no longer installed.
 *
 *   motion_check [captures]
 *
 * Three environment variables put it inside the stress mix, which is how
 * validate.sh asks the same question of a compositor that is busy: BENCH_ABOVE
 * keeps the window on top, BENCH_SECONDS caps how long it looks and spreads the
 * captures over that time, and BENCH_GO holds it at the starting gate.
 *
 * Exit status: 0 every capture was one clean frame, 1 one was not, 2 it could
 * not run, 3 it was covered throughout and proved nothing. See the README.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"
#include "polite.h"
#include "gate.h"
#include "now.h"

/* Seconds to keep looking, whatever the count, or none. In the stress mix the
   load ends on the clock and there is no point capturing an idle screen after
   it. */
static double bench_seconds (void)
{
    const char *e = getenv ("BENCH_SECONDS");

    return (e != NULL && *e != '\0') ? atof (e) : 0.0;
}

#define BAND    40
#define NCOL    8
#define WINW    900
#define WINH    720
#define MARGIN  20

static const unsigned long palette[NCOL] = {
    0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    0xff00ff, 0x00ffff, 0xffffff, 0x808080
};

/* Which colour a row shows when the pattern is scrolled by offset */
static int band_of (int y, int offset)
{
    int k = (y + offset) / BAND;

    return ((k % NCOL) + NCOL) % NCOL;
}

static int colour_index (unsigned long pixel)
{
    int i;

    for (i = 0; i < NCOL; i++)
    {
        if ((pixel & 0xffffff) == palette[i])
        {
            return i;
        }
    }

    return -1;
}

int main (int argc, char **argv)
{
    Display *d;
    Window win, root;
    GC gc;
    XImage *img;
    XWindowAttributes wa;
    int scr, want = (argc > 1) ? atoi (argv[1]) : 0;
    int captures = 0, clean = 0, torn = 0, unknown = 0;
    int offset = 0, i, x, y, ox, oy;
    double limit, t0, pace, due;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    win = XCreateSimpleWindow (d, root, 100, 100, WINW, WINH, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    XStoreName (d, win, "motion_check");
    /*
     * In the middle of the stress mix this window has to stay on top: covered,
     * every capture is of somebody else's pixels and proves nothing.
     */
    if (getenv ("BENCH_ABOVE") != NULL)
    {
        polite_keep_above (d, win);
    }
    XSelectInput (d, win, ExposureMask | StructureNotifyMask);
    XMapRaised (d, win);
    gc = XCreateGC (d, win, 0, NULL);

    /* Let the window manager finish framing and mapping it */
    XSync (d, False);
    sleep (2);
    XRaiseWindow (d, win);
    polite_activate (d, root, win);
    XSync (d, False);
    sleep (1);

    /* Where the window really ended up, the frame having moved it */
    {
        Window child;

        XGetWindowAttributes (d, win, &wa);
        XTranslateCoordinates (d, win, root, 0, 0, &ox, &oy, &child);
    }

    /* Everything is up: in a mix, start when the rest of the load does */
    bench_wait_go ();
    limit = bench_seconds ();
    if (want <= 0)
    {
        want = (limit > 0) ? INT_MAX : 30;
    }
    /*
     * Given both a count and a time, spread the captures over the time. Taken
     * flat out they are a load in themselves - a whole region read back from
     * the server each time - and in the stress mix that would be weight added
     * to the very thing being watched.
     */
    pace = (limit > 0 && want < INT_MAX) ? limit / want : 0;
    t0 = bench_now ();

    while (captures < want && (limit <= 0 || bench_now () - t0 < limit))
    {
        int fits = -1;

        /*
         * Draw the pattern at this offset and let the server have it. The
         * bands start at -(offset % BAND) so that every row r really does
         * show palette[band_of (r, offset)], which is what the check below
         * asserts.
         */
        {
            int start = -(offset % BAND);
            int j = 0;

            for (y = start; y < WINH; y += BAND, j++)
            {
                XSetForeground (d, gc,
                                palette[((offset / BAND + j) % NCOL)]);
                XFillRectangle (d, win, gc, 0, y, WINW, BAND);
            }
        }
        XFlush (d);
        usleep (25000);

        img = capture_region (d, root, ox + MARGIN, oy + MARGIN,
                         WINW - 2 * MARGIN, WINH - 2 * MARGIN);
        if (img == NULL)
        {
            fprintf (stderr, "capture failed\n");
            break;
        }
        captures++;

        /*
         * A clean capture is one whole frame: some single offset explains
         * every row of it. Anything else is a mix of two frames.
         */
        for (i = 0; i < BAND * NCOL && fits < 0; i++)
        {
            int ok = 1;

            for (y = 0; y < img->height && ok; y += 4)
            {
                for (x = 0; x < 3; x++)
                {
                    int px = (img->width / 4) * (x + 1);
                    int c = colour_index (XGetPixel (img, px, y));

                    if (c < 0 || c != band_of (y + MARGIN, i))
                    {
                        ok = 0;
                        break;
                    }
                }
            }
            if (ok)
            {
                fits = i;
            }
        }

        if (fits >= 0)
        {
            clean++;
        }
        else
        {
            /* Tell an unpainted or foreign capture from a genuinely mixed one */
            int known = 0, total = 0;

            for (y = 0; y < img->height; y += 8)
            {
                total++;
                if (colour_index (XGetPixel (img, img->width / 2, y)) >= 0)
                {
                    known++;
                }
            }
            if (known < total)
            {
                unknown++;
            }
            else
            {
                torn++;
                printf ("capture %d: mixed offsets\n", captures);
            }
        }

        XDestroyImage (img);
        offset += BAND / 4;

        /* Wait for this capture's slot to be over before taking the next */
        due = t0 + captures * pace;
        if (pace > 0 && due > bench_now ())
        {
            usleep ((useconds_t) ((due - bench_now ()) * 1e6));
        }
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("%d captures: %d clean, %d torn or stale, %d not our pattern\n",
            captures, clean, torn, unknown);

    if (torn > 0)
    {
        return 1;
    }
    /* Nothing but somebody else's pixels: covered the whole time, in the
       stress mix most likely, and no answer either way. See validate.sh. */
    if (clean == 0)
    {
        return (unknown > 0) ? 3 : 1;
    }

    return 0;
}
