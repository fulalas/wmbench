/*
 * A frame drawn from a pixmap that was not ready yet, caught the way
 * motion_check catches tearing rather than by trying to photograph a black
 * frame in the act.
 *
 * A window is given a band pattern whose offset advances every step, and it is
 * resized every step as well, which is what hands it a new pixmap. A fixed
 * region well inside the smallest size it takes is captured each time. Every
 * capture has to be explainable by one single offset of the pattern: a frame
 * drawn from an unready pixmap is not, because it holds something that is not
 * the pattern at any offset.
 *
 * This is the detector the previous attempt lacked. Photographing the screen in
 * a tight loop never saw anything, in any present mode, with or without the
 * read that is supposed to prevent it, so it proved nothing. A pattern that is
 * always moving does not have to be caught at the right instant.
 *
 *   resize_check [steps] [managed]
 *
 * The managed mode lets the window manager frame the window, which is the case
 * the comment on wait_for_pixmap() was measuring; the window's position moves as
 * it is reframed, so it is looked up every step.
 *
 * Exit status 0 when every capture was one coherent frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"
#include <X11/Xatom.h>

#define BAND     32
#define NCOL      8
#define BASEW   520
#define BASEH   420
#define GROW    240
#define INSET    40            /* the captured region, inside every size */

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
    Window root, win;
    GC gc;
    XSetWindowAttributes swa;
    XImage *img;
    int scr, steps = (argc > 1) ? atoi (argv[1]) : 120;
    int managed = (argc > 2 && !strcmp (argv[2], "managed"));
    int s, i, x, y, px, py, offset = 0;
    int clean = 0, incoherent = 0, foreign = 0, edge_bad = 0, blind = 0;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);
    px = (DisplayWidth (d, scr) - BASEW) / 2;
    py = (DisplayHeight (d, scr) - BASEH) / 2;

    /* Override redirect: no frame, no manager, so the resize is immediate */
    memset (&swa, 0, sizeof swa);
    swa.override_redirect = managed ? False : True;
    swa.background_pixel = BlackPixel (d, scr);
    win = XCreateWindow (d, root, px, py, BASEW, BASEH, 0, CopyFromParent,
                         InputOutput, CopyFromParent,
                         CWOverrideRedirect | CWBackPixel, &swa);
    if (managed)
    {
        Atom wt = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
        Atom nm = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);

        XChangeProperty (d, win, wt, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *) &nm, 1);
        XStoreName (d, win, "resize_check");
    }
    XMapRaised (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (2);

    /*
     * Warm up unscored. On a slow machine the first capture can race the
     * window's first appearance and read as "not the pattern", which is a
     * startup race and not a compositor defect; it cost a false alarm on the
     * NVIDIA machine once. Three full cycles are drawn and thrown away.
     */
    for (s = -3; s < steps; s++)
    {
        int w = BASEW + ((s % 2) ? GROW : 0);
        int h = BASEH + ((s % 2) ? GROW / 2 : 0);
        int start, j, fits = -1;

        XResizeWindow (d, win, w, h);
        XRaiseWindow (d, win);
        if (managed)
        {
            /*
             * The manager applies the resize in its own time and may move the
             * client while reframing it, so the size and position have to be
             * read back rather than assumed. Capturing an area the window does
             * not occupy yet looks exactly like a defect and is not one.
             */
            XWindowAttributes wa;
            Window ch;

            XSync (d, False);
            usleep (25000);
            if (!XGetWindowAttributes (d, win, &wa))
            {
                continue;
            }
            w = wa.width;
            h = wa.height;
            XTranslateCoordinates (d, win, root, 0, 0, &px, &py, &ch);
        }

        /* The pattern, at this step's offset, over the whole new size */
        start = -(offset % BAND);
        for (y = start, j = 0; y < h; y += BAND, j++)
        {
            XSetForeground (d, gc, palette[(offset / BAND + j) % NCOL]);
            XFillRectangle (d, win, gc, 0, y, w, BAND);
        }
        XFlush (d);
        usleep (9000);

        /*
         * A region inside the smallest size the window ever takes, so it is
         * window whichever size is currently on screen. That is what makes
         * this usable while the compositor lags the server under load: there
         * is no moment when the captured area is not the window. Capturing
         * the full current size instead reads as a defect every time the
         * compositor is one frame behind, which under load is 20% of resizes.
         */
        img = capture_region (d, root, px + INSET, py + INSET,
                         BASEW - 2 * INSET, BASEH - 2 * INSET);
        if (img == NULL)
        {
            continue;
        }


        /* Is there one offset that explains the whole capture? */
        for (i = 0; i < BAND * NCOL && fits < 0; i++)
        {
            int ok = 1;

            for (y = 0; y < img->height && ok; y += 2)
            {
                for (x = 0; x < 4; x++)
                {
                    int cx = (img->width / 5) * (x + 1);

                    if (colour_index (XGetPixel (img, cx, y)) !=
                        band_of (y + INSET, i))
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

        /*
         * The edges as well, at the size the window has right now. The inset
         * capture above cannot see a band painted along an edge - a
         * compositor that samples past the end of a window pixmap while it
         * grows paints one down the right side, full height, and the check
         * called every resize coherent. Only pixels that are no colour of
         * the pattern count here, so a compositor merely a frame behind
         * (still showing the pattern, just an older offset) is not accused.
         */
        if (s >= 0)
        {
            XImage *edge = capture_region (d, root, px, py,
                                           (unsigned) w, (unsigned) h);

            if (edge != NULL)
            {
                int fx, fy, bad = 0;
                int minx = edge->width, maxx = -1;
                int miny = edge->height, maxy = -1;

                for (fy = 2; fy < edge->height - 2; fy += 3)
                {
                    for (fx = 2; fx < edge->width - 2; fx += 3)
                    {
                        if (colour_index (XGetPixel (edge, fx, fy)) < 0)
                        {
                            bad++;
                            if (fx < minx) { minx = fx; }
                            if (fx > maxx) { maxx = fx; }
                            if (fy < miny) { miny = fy; }
                            if (fy > maxy) { maxy = fy; }
                        }
                    }
                }
                /*
                 * A handful of pixels are the window manager's own frame
                 * showing through at the very edge; a band is thousands.
                 */
                if (bad > 200)
                {
                    /*
                     * Where they are says what they are. A band lies along one
                     * edge, so it is narrow one way however long it is the
                     * other. Wrong pixels spread across most of the window in
                     * both directions are not a band at all: another window is
                     * sitting on top of ours, and a photograph of someone
                     * else's window says nothing about this one. Nothing here
                     * owns our stacking, so it can happen at any time.
                     */
                    if (maxx - minx > (edge->width * 3) / 5 &&
                        maxy - miny > (edge->height * 3) / 5)
                    {
                        blind++;
                        printf ("step %d: something is covering the window, "
                                "nothing proved\n", s);
                    }
                    else
                    {
                        edge_bad++;
                        printf ("step %d: %d pixels inside the window are no "
                                "colour of the pattern\n", s, bad);
                    }
                }
                XDestroyImage (edge);
            }
        }

        if (s < 0)
        {
            /* warmup, unscored */
        }
        else if (fits >= 0)
        {
            clean++;
        }
        else
        {
            int known = 0, total = 0;

            for (y = 0; y < img->height; y += 6)
            {
                total++;
                if (colour_index (XGetPixel (img, img->width / 2, y)) >= 0)
                {
                    known++;
                }
            }
            if (known == 0)
            {
                /*
                 * Not one row of ours anywhere: we are photographing another
                 * window, not a defect in this one. See the same reasoning at
                 * the edge test below.
                 */
                blind++;
                printf ("step %d: something is covering the window, "
                        "nothing proved\n", s);
            }
            else if (known < total)
            {
                /* Something that is not the pattern at all: black, or garbage */
                foreign++;
                printf ("step %d: %d of %d rows are not the pattern at all\n",
                        s, total - known, total);
            }
            else
            {
                incoherent++;
                printf ("step %d: pattern colours but no single offset fits\n", s);
            }
        }
        XDestroyImage (img);
        offset += BAND / 4;
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("resizes: %d coherent, %d mixed, %d not the pattern, "
            "%d with a band at an edge, %d proved nothing\n",
            clean, incoherent, foreign, edge_bad, blind);

    return (incoherent == 0 && foreign == 0 && edge_bad == 0 && clean > 0)
           ? 0 : 1;
}
