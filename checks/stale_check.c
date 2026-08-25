/*
 * The swap presentation paints only the damage and trusts the buffer age to
 * say what the buffer it was handed still holds. If that trust is misplaced
 * the screen keeps pixels from an older frame. This looks for exactly that,
 * in the two ways the retired python checks did:
 *
 *   settled   scroll a known pattern hard, stop, and compare what is on the
 *             screen against the pattern that should be there. Leftovers from
 *             an earlier frame show up as bands of the wrong colour.
 *   stale     photograph a window that is not being drawn to, damage a
 *             different window heavily for a while, and photograph again. The
 *             untouched window must be unchanged, and must still match the
 *             pattern it was given.
 *
 * Replaces scroll_check.py and stale_check.py, whose python-xlib and PIL are
 * no longer installed.
 *
 *   stale_check [rounds]
 *
 * Exit status 0 when nothing went stale.
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
#define WINH    700
#define MARGIN  20
#define STEP    7

static const unsigned long palette[NCOL] = {
    0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    0xff00ff, 0x00ffff, 0xffffff, 0x808080
};

static int band_of (int y, int offset)
{
    return (((y + offset) / BAND) % NCOL + NCOL) % NCOL;
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

static void draw_pattern (Display *d, Window w, GC gc, int offset)
{
    int start = -(offset % BAND);
    int j = 0, y;

    for (y = start; y < WINH; y += BAND, j++)
    {
        XSetForeground (d, gc, palette[(offset / BAND + j) % NCOL]);
        XFillRectangle (d, w, gc, 0, y, WINW, BAND);
    }
}

/* Every sampled pixel must be the colour this offset calls for */
static int matches (XImage *img, int offset, int *bad_row)
{
    int x, y;

    for (y = 0; y < img->height; y += 2)
    {
        for (x = 0; x < 5; x++)
        {
            int px = (img->width / 6) * (x + 1);

            if (colour_index (XGetPixel (img, px, y)) !=
                band_of (y + MARGIN, offset))
            {
                *bad_row = y;

                return 0;
            }
        }
    }

    return 1;
}

int main (int argc, char **argv)
{
    Display *d;
    Window root, w1, w2, child;
    GC gc1, gc2;
    XImage *img;
    int scr, rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, i, ox, oy, bad = 0;
    int settled_ok = 0, settled_bad = 0, stale_ok = 0, stale_bad = 0;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    w1 = XCreateSimpleWindow (d, root, 100, 100, WINW, WINH, 0,
                              BlackPixel (d, scr), BlackPixel (d, scr));
    w2 = XCreateSimpleWindow (d, root, 100 + WINW + 120, 100, WINW, WINH, 0,
                              BlackPixel (d, scr), BlackPixel (d, scr));
    XStoreName (d, w1, "stale_check pattern");
    XStoreName (d, w2, "stale_check noise");

    /*
     * Without position hints the window manager places these itself, and it
     * happily puts the noise window on top of the pattern one, at which point
     * the capture is of the wrong window and everything looks stale. The size
     * is asked for the same way and for the same reason: every capture below
     * reads a fixed WINW x WINH region, so a window the manager resized is
     * photographed along with whatever is beside it, which reads as stale.
     */
    {
        XSizeHints h;

        h.flags = USPosition | USSize | PPosition | PSize |
                  PMinSize | PMaxSize;
        h.width = h.min_width = h.max_width = WINW;
        h.height = h.min_height = h.max_height = WINH;
        h.x = 100; h.y = 100;
        XSetWMNormalHints (d, w1, &h);
        h.x = 100 + WINW + 120;
        XSetWMNormalHints (d, w2, &h);
    }

    XMapWindow (d, w2);
    XMapWindow (d, w1);
    gc1 = XCreateGC (d, w1, 0, NULL);
    gc2 = XCreateGC (d, w2, 0, NULL);
    XSync (d, False);
    sleep (3);
    /* The pattern window must be the visible one wherever they ended up */
    XRaiseWindow (d, w1);
    polite_activate (d, root, w1);
    XSync (d, False);
    sleep (1);

    XTranslateCoordinates (d, w1, root, 0, 0, &ox, &oy, &child);
    {
        int x2, y2;

        XTranslateCoordinates (d, w2, root, 0, 0, &x2, &y2, &child);
        printf ("pattern at %d,%d  noise at %d,%d\n", ox, oy, x2, y2);
        if (ox < x2 + WINW && x2 < ox + WINW &&
            oy < y2 + WINH && y2 < oy + WINH)
        {
            printf ("the two windows overlap, the check would be meaningless\n");

            return 2;
        }
    }

    for (r = 0; r < rounds; r++)
    {
        int offset = 0;

        /*
         * Settled. Scroll hard, so the damage is large every frame and the
         * buffer age is being leaned on, then stop and look.
         */
        for (i = 0; i < 150; i++)
        {
            draw_pattern (d, w1, gc1, offset);
            XFlush (d);
            usleep (4000);
            offset += STEP;
        }
        /* Land on a whole band so the expected picture is unambiguous */
        offset += BAND - (offset % BAND);
        draw_pattern (d, w1, gc1, offset);
        XSync (d, False);
        usleep (600000);

        for (i = 0; i < 3; i++)
        {
            img = capture_region (d, root, ox + MARGIN, oy + MARGIN,
                             WINW - 2 * MARGIN, WINH - 2 * MARGIN);
            if (img == NULL)
            {
                fprintf (stderr, "capture failed\n");

                return 2;
            }
            if (matches (img, offset, &bad))
            {
                settled_ok++;
            }
            else
            {
                settled_bad++;
                printf ("round %d: settled capture wrong from row %d\n",
                        r + 1, bad);
            }
            XDestroyImage (img);
            usleep (150000);
        }

        /*
         * Stale. Nothing touches the pattern window now; the other window is
         * damaged as fast as it can be. If presenting only the damage leaves
         * anything behind, the pattern window is where it will show.
         */
        for (i = 0; i < 400; i++)
        {
            XSetForeground (d, gc2, palette[i % NCOL]);
            XFillRectangle (d, w2, gc2, (i * 37) % 700, (i * 53) % 500,
                            200, 200);
            if ((i % 40) == 0)
            {
                XFlush (d);
                usleep (10000);
            }
        }
        XSync (d, False);
        usleep (400000);

        img = capture_region (d, root, ox + MARGIN, oy + MARGIN,
                         WINW - 2 * MARGIN, WINH - 2 * MARGIN);
        if (img == NULL)
        {
            fprintf (stderr, "capture failed\n");

            return 2;
        }
        if (matches (img, offset, &bad))
        {
            stale_ok++;
        }
        else
        {
            stale_bad++;
            printf ("round %d: window went stale from row %d\n",
                    r + 1, bad);
        }
        XDestroyImage (img);
    }


    XDestroyWindow (d, w1);
    XDestroyWindow (d, w2);
    XCloseDisplay (d);

    printf ("settled %d ok %d wrong, stale %d ok %d wrong\n",
            settled_ok, settled_bad, stale_ok, stale_bad);

    return (settled_bad == 0 && stale_bad == 0 &&
            settled_ok > 0 && stale_ok > 0) ? 0 : 1;
}
