/*
 * Does what a menu covered come back correctly?
 *
 * stale_check covers a window nobody is drawing to while another is hammered,
 * and motion_check covers tearing during motion, but neither covers a window
 * appearing over another and going away again. That is the commonest thing a
 * compositor does after drawing: every menu, tooltip and notification. The
 * area a popup covered has to be repainted from the window underneath when it
 * unmaps, and nothing here has ever checked that it is.
 *
 * A known band pattern sits in a window. Override-redirect popups, the way
 * real menus are, are mapped over it and unmapped again, many times. Then the
 * pattern window is captured and must still be exactly the pattern.
 *
 *   pop_check [rounds]
 *
 * Exit status 0 when nothing was left behind.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"

#define BAND    40
#define NCOL    8
#define WINW    900
#define WINH    700
#define MARGIN  20
#define POPW    260
#define POPH    300

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

int main (int argc, char **argv)
{
    Display *d;
    Window root, win, pop[3];
    GC gc;
    XImage *img;
    XSizeHints hints;
    XSetWindowAttributes swa;
    int scr, rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, i, j, x, y, ox, oy, ok = 0, bad = 0;
    Window child;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    win = XCreateSimpleWindow (d, root, 120, 120, WINW, WINH, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = 120; hints.y = 120; hints.width = WINW; hints.height = WINH;
    XSetWMNormalHints (d, win, &hints);
    XStoreName (d, win, "pop_check pattern");
    XMapWindow (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (3);

    XTranslateCoordinates (d, win, root, 0, 0, &ox, &oy, &child);

    /* Popups land on top of the pattern, which is the whole point */
    swa.override_redirect = True;
    for (i = 0; i < 3; i++)
    {
        pop[i] = XCreateWindow (d, root, ox + 60 + i * 200, oy + 80 + i * 90,
                                POPW, POPH, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWOverrideRedirect, &swa);
    }
    XSync (d, False);

    for (r = 0; r < rounds; r++)
    {
        /* Draw the pattern, then cover and uncover it repeatedly */
        for (y = 0; y < WINH; y += BAND)
        {
            XSetForeground (d, gc, palette[(y / BAND) % NCOL]);
            XFillRectangle (d, win, gc, 0, y, WINW, BAND);
        }
        XSync (d, False);
        usleep (200000);

        for (i = 0; i < 24; i++)
        {
            Window w = pop[i % 3];

            XMapRaised (d, w);
            for (j = 0; j < 6; j++)
            {
                XSetForeground (d, gc, palette[(i + j) % NCOL]);
                XFillRectangle (d, w, gc, 8, 8 + j * 46, POPW - 16, 40);
            }
            XSync (d, False);
            usleep (25000);
            XUnmapWindow (d, w);
            XSync (d, False);
            usleep (25000);
        }

        /* Everything is unmapped again; the pattern must be untouched */
        usleep (500000);
        img = capture_region (d, root, ox + MARGIN, oy + MARGIN,
                         WINW - 2 * MARGIN, WINH - 2 * MARGIN);
        if (img == NULL)
        {
            fprintf (stderr, "capture failed\n");

            return 2;
        }

        {
            int wrong = -1;

            for (y = 0; y < img->height && wrong < 0; y += 2)
            {
                for (x = 0; x < 5; x++)
                {
                    int px = (img->width / 6) * (x + 1);

                    if (colour_index (XGetPixel (img, px, y)) !=
                        band_of (y + MARGIN, 0))
                    {
                        wrong = y;
                        break;
                    }
                }
            }
            if (wrong < 0)
            {
                ok++;
            }
            else
            {
                bad++;
                printf ("round %d: a popup left something behind at row %d\n",
                        r + 1, wrong);
            }
        }
        XDestroyImage (img);
    }

    for (i = 0; i < 3; i++)
    {
        XDestroyWindow (d, pop[i]);
    }
    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("popups: %d rounds clean, %d left something behind\n", ok, bad);

    return (bad == 0 && ok > 0) ? 0 : 1;
}
