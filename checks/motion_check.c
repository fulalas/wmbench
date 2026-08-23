/*
 * Captures the screen WHILE a known pattern scrolls, so corruption that only
 * exists during motion is seen. Every capture must show one single offset of
 * the pattern; a capture mixing two offsets is tearing or stale pixels.
 *
 * Replaces motion_check.py, whose python-xlib and PIL are no longer installed.
 *
 *   motion_check [captures]
 *
 * Exit status 0 when every capture was one clean frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"
#include "polite.h"

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
    int scr, want = (argc > 1) ? atoi (argv[1]) : 30;
    int captures = 0, clean = 0, torn = 0, unknown = 0;
    int offset = 0, i, x, y, ox, oy;

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

    while (captures < want)
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
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("%d captures: %d clean, %d torn or stale, %d not our pattern\n",
            captures, clean, torn, unknown);

    return (torn == 0 && clean > 0) ? 0 : 1;
}
