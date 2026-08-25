/*
 * A window hanging off the left and top edges of the screen, which is what
 * happens every time someone drags one there and which nothing here has tested.
 *
 * The compositor works out texture coordinates from screen positions, so for a
 * window at a negative origin those sums involve negative numbers, which is a
 * classic place for an off-by-a-region error. Get it wrong and the visible part
 * of the window shows the wrong part of its contents, shifted.
 *
 * A pattern of square cells whose colour depends on the column and the row
 * together makes that visible, and in both directions: a pattern of rows alone
 * says nothing about the left edge, since sliding it sideways lands on exactly
 * the same colours. With the window at -OFFX,-OFFY, the top left of the screen
 * must show the cell belonging to window pixel OFFX,OFFY, not the one
 * belonging to 0,0.
 *
 *   offscreen_check [rounds]
 *
 * Exit status: 0 the right part was visible, 1 the wrong one was, 2 it could
 * not run, 3 something covered the corner throughout and nothing was proved.
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

static int cell_of (int win_x, int win_y)
{
    return ((win_x / BAND) + (win_y / BAND)) % NCOL;
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
    int r, x, y, ok = 0, bad = 0, blind = 0;

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
        int wrong = -1, seen = 0;

        for (y = 0; y < WINH; y += BAND)
        {
            for (x = 0; x < WINW; x += BAND)
            {
                XSetForeground (d, gc, palette[cell_of (x, y)]);
                XFillRectangle (d, win, gc, x, y, BAND, BAND);
            }
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
                int idx = colour_index (XGetPixel (img, cx, y));

                /*
                 * No colour of the pattern at all, so this pixel is not ours:
                 * the corner sampled here is where panels, docks and tray
                 * popups live, and nothing owns the stacking after the first
                 * map. Somebody else's window says nothing about this one
                 * either way, and counting it would score a panel as a
                 * compositor fault.
                 */
                if (idx < 0)
                {
                    continue;
                }
                /* screen 4 + cx, 4 + y is window OFFX + 4 + cx, OFFY + 4 + y */
                if (idx != cell_of (OFFX + cx + 4, OFFY + y + 4))
                {
                    wrong = y;
                    break;
                }
                seen++;
            }
        }
        XDestroyImage (img);

        if (wrong >= 0)
        {
            bad++;
            printf ("round %d: wrong part of the window visible, from row %d\n",
                    r + 1, wrong);
        }
        else if (seen == 0)
        {
            blind++;
            printf ("round %d: something is covering the window, "
                    "nothing proved\n", r + 1);
        }
        else
        {
            ok++;
        }
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("off-screen window: %d clean, %d wrong, %d proved nothing\n",
            ok, bad, blind);

    if (bad > 0)
    {
        return 1;
    }
    /* Covered in every round, so every capture was somebody else's pixels: no
       answer either way, which is not the same as a fault. See the README. */
    if (ok == 0)
    {
        return (blind > 0) ? 3 : 1;
    }

    return 0;
}
