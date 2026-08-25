/*
 * Minimising a window and bringing it back, which nothing here has tested.
 *
 * It is a different path from a menu appearing. The window goes on existing, but
 * it is unmapped, so the compositor frees its pixmap, its GLX pixmap and its
 * texture; restoring it builds all three again for a window it already knows
 * about. Everyone minimises windows, and a leftover or a black restore would be
 * obvious.
 *
 * A band pattern makes it checkable. The check proves the minimise happened
 * rather than assuming it: after minimising, the pattern must be gone from where
 * it was, and a round where it is still there is reported as having proved
 * nothing.
 *
 *   iconify_check [rounds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"
#include <X11/Xatom.h>

#define BAND    40
#define NCOL     8
#define WINW   900
#define WINH   700
#define MARGIN  30

static const unsigned long palette[NCOL] = {
    0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    0xff00ff, 0x00ffff, 0xffffff, 0x808080
};

static int band_of (int y)
{
    return (y / BAND) % NCOL;
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

/* How many sampled points look like the pattern they should be */
static int pattern_score (Display *d, Window root, int ox, int oy, int *total)
{
    XImage *img;
    int x, y, hit = 0, n = 0;

    img = capture_region (d, root, ox + MARGIN, oy + MARGIN,
                     WINW - 2 * MARGIN, WINH - 2 * MARGIN);
    if (img == NULL)
    {
        *total = 0;

        return -1;
    }
    for (y = 0; y < img->height; y += 4)
    {
        for (x = 0; x < 4; x++)
        {
            int cx = (img->width / 5) * (x + 1);

            n++;
            if (colour_index (XGetPixel (img, cx, y)) == band_of (y + MARGIN))
            {
                hit++;
            }
        }
    }
    XDestroyImage (img);
    *total = n;

    return hit;
}

int main (int argc, char **argv)
{
    Display *d;
    Window root, win, child;
    GC gc;
    XSizeHints hints;
    Atom wtype, normal;
    int scr, rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, y, ox, oy, ok = 0, bad = 0, inconclusive = 0, nocapture = 0;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    win = XCreateSimpleWindow (d, root, 150, 150, WINW, WINH, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = 150; hints.y = 150; hints.width = WINW; hints.height = WINH;
    XSetWMNormalHints (d, win, &hints);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, win, "iconify_check");
    XSelectInput (d, win, StructureNotifyMask);
    XMapWindow (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (3);

    XTranslateCoordinates (d, win, root, 0, 0, &ox, &oy, &child);

    for (r = 0; r < rounds; r++)
    {
        int hit, total, gone_hit, gone_total;

        for (y = 0; y < WINH; y += BAND)
        {
            XSetForeground (d, gc, palette[band_of (y)]);
            XFillRectangle (d, win, gc, 0, y, WINW, BAND);
        }
        XSync (d, False);
        usleep (500000);

        hit = pattern_score (d, root, ox, oy, &total);
        /* A total of zero is no photograph at all, which says nothing about
           the pattern and has to be kept apart from a pattern that is wrong */
        if (total == 0)
        {
            printf ("round %d: the screen cannot be photographed\n", r + 1);
            nocapture++;
            continue;
        }
        if (hit < total)
        {
            printf ("round %d: the pattern is not on screen to begin with "
                    "(%d of %d), so nothing can be concluded\n",
                    r + 1, hit, total);
            inconclusive++;
            continue;
        }

        /* Minimise */
        XIconifyWindow (d, win, scr);
        XSync (d, False);
        usleep (900000);

        gone_hit = pattern_score (d, root, ox, oy, &gone_total);
        if (gone_total == 0)
        {
            printf ("round %d: the screen cannot be photographed after "
                    "minimising\n", r + 1);
            nocapture++;
            XMapWindow (d, win);
            XSync (d, False);
            usleep (700000);
            continue;
        }
        if (gone_hit == gone_total)
        {
            printf ("round %d: still fully there after minimising, so the "
                    "minimise did not happen and this proves nothing\n", r + 1);
            inconclusive++;
            /* Put it back anyway before the next round */
            XMapWindow (d, win);
            XSync (d, False);
            usleep (700000);
            continue;
        }

        /* Restore, then redraw: unmapped windows are not kept by the server */
        XMapWindow (d, win);
        XSync (d, False);
        usleep (900000);
        for (y = 0; y < WINH; y += BAND)
        {
            XSetForeground (d, gc, palette[band_of (y)]);
            XFillRectangle (d, win, gc, 0, y, WINW, BAND);
        }
        XSync (d, False);
        usleep (700000);

        /* The manager may have put it somewhere else on restoring */
        XTranslateCoordinates (d, win, root, 0, 0, &ox, &oy, &child);
        hit = pattern_score (d, root, ox, oy, &total);

        if (total == 0)
        {
            printf ("round %d: the screen cannot be photographed after "
                    "restoring\n", r + 1);
            nocapture++;
        }
        else if (hit == total)
        {
            ok++;
        }
        else
        {
            bad++;
            printf ("round %d: after restoring, %d of %d sampled points are "
                    "wrong\n", r + 1, total - hit, total);
        }
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("minimise and restore: %d clean, %d wrong, %d proved nothing\n",
            ok, bad, inconclusive);

    /* 3 keeps a round that proved nothing apart from a round that went wrong,
       and only when no round proved anything at all */
    if (bad > 0)
        return 1;
    /* A round nobody could photograph is this check failing to look, not the
       compositor failing, and validate.sh keeps the two apart */
    if (ok == 0 && nocapture > inconclusive)
    {
        printf ("%d of %d rounds could not be photographed at all\n",
                nocapture, rounds);

        return 2;
    }
    if (ok == 0)
        return (inconclusive > 0) ? 3 : 1;

    return 0;
}
