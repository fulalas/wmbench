/*
 * A window hanging off the left and top edges of the screen, which is what
 * happens every time someone drags one there and which nothing here has tested.
 *
 * The compositor works out texture coordinates from screen positions, so for a
 * window at a negative origin those sums involve negative numbers, which is a
 * classic place for an off-by-a-region error. Get it wrong and the visible part
 * of the window shows the wrong part of its contents, shifted.
 *
 * A band pattern whose colour depends only on the row makes that visible. With
 * the window at y = -OFFY, the top of the screen must show the band belonging to
 * window row OFFY, not the one belonging to row 0.
 *
 *   offscreen_check [rounds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"

#define BAND    40
#define NCOL     8
#define WINW  1000
#define WINH   800
#define OFFX   240             /* how far off the left edge */
#define OFFY   200             /* how far off the top edge */

static const unsigned long palette[NCOL] = {
    0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    0xff00ff, 0x00ffff, 0xffffff, 0x808080
};

static int band_of (int win_y)
{
    return (win_y / BAND) % NCOL;
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
    Window root, win;
    GC gc;
    XSetWindowAttributes swa;
    XImage *img;
    int scr, rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, x, y, ok = 0, bad = 0;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    /* Override redirect: a manager would refuse to place this off screen */
    memset (&swa, 0, sizeof swa);
    swa.override_redirect = True;
    swa.background_pixel = BlackPixel (d, scr);
    win = XCreateWindow (d, root, -OFFX, -OFFY, WINW, WINH, 0, CopyFromParent,
                         InputOutput, CopyFromParent,
                         CWOverrideRedirect | CWBackPixel, &swa);
    XMapRaised (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (2);

    for (r = 0; r < rounds; r++)
    {
        int wrong = -1;

        for (y = 0; y < WINH; y += BAND)
        {
            XSetForeground (d, gc, palette[band_of (y)]);
            XFillRectangle (d, win, gc, 0, y, WINW, BAND);
        }
        XSync (d, False);
        usleep (500000);

        /*
         * The visible part starts at screen 0,0 and is window pixel OFFX,OFFY.
         * Sampling starts a few pixels in so the very edge is not what decides
         * the result.
         */
        img = capture_region (d, root, 4, 4, WINW - OFFX - 40, WINH - OFFY - 40);
        if (img == NULL)
        {
            fprintf (stderr, "capture failed\n");

            return 2;
        }
        for (y = 0; y < img->height && wrong < 0; y += 2)
        {
            for (x = 0; x < 4; x++)
            {
                int cx = (img->width / 5) * (x + 1);

                /* screen row y + 4 is window row OFFY + y + 4 */
                if (colour_index (XGetPixel (img, cx, y)) !=
                    band_of (OFFY + y + 4))
                {
                    wrong = y;
                    break;
                }
            }
        }
        XDestroyImage (img);

        if (wrong < 0)
        {
            ok++;
        }
        else
        {
            bad++;
            printf ("round %d: wrong part of the window visible, from row %d\n",
                    r + 1, wrong);
        }
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("off-screen window: %d clean, %d wrong\n", ok, bad);

    return (bad == 0 && ok > 0) ? 0 : 1;
}
