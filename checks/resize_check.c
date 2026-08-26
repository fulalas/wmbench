/*
 *   resize_check [steps] [managed]
 *
 * A frame drawn from a buffer that was not ready yet. The window carries a band
 * pattern whose offset advances every step and is resized every step, which is
 * what hands it a new buffer, and a fixed region well inside its smallest size
 * is captured each time. Every capture has to be explainable by one single
 * offset: a frame drawn from an unready buffer is not. A pattern that is always
 * moving does not have to be caught at the right instant, which is why
 * photographing in a tight loop proved nothing.
 *
 * The managed mode lets the window manager frame the window; its position moves
 * as it is reframed, so it is looked up every step.
 *
 * Exit status: 0 every capture was one coherent frame, 1 one was not, 2 it
 * could not run, 3 it was covered throughout and proved nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win.h"
#include "place.h"

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
    bw_win *win;
    bw_image *img;
    int steps = (argc > 1) ? atoi (argv[1]) : 120;
    int managed = (argc > 2 && !strcmp (argv[2], "managed"));
    int sw, sh, s, i, x, y, px, py, offset = 0;
    int clean = 0, incoherent = 0, foreign = 0, edge_bad = 0, blind = 0;
    int no_capture = 0;

    /* Zero steps proves nothing and must not be reported as a failure */
    if (steps < 1)
    {
        fprintf (stderr, "steps must be at least 1\n");

        return 2;
    }
    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    bw_screen_size (&sw, &sh);
    px = (sw - BASEW) / 2;
    py = (sh - BASEH) / 2;

    /* Unmanaged by default: no frame, no manager, so the resize is immediate */
    win = bw_create (NULL, px, py, BASEW, BASEH,
                     managed ? "resize_check" : NULL,
                     managed ? BW_LOOSE : BW_POPUP);
    if (win == NULL)
    {
        fprintf (stderr, "no window\n");
        bw_close ();

        return 2;
    }
    if (!bench_aimable (win))
    {
        printf ("the screen cannot be photographed here, nothing proved\n");
        bw_destroy (win);
        bw_close ();

        return 3;
    }
    bw_map (win);
    bw_raise (win);
    bw_sync ();
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
        int start, j, fits = -1, covered = 0;
        int iw, ih;

        bw_resize (win, w, h);
        bw_raise (win);
        if (managed)
        {
            /*
             * The manager applies the resize in its own time and may move the
             * client while reframing it, so the size and position have to be
             * read back rather than assumed. Capturing an area the window does
             * not occupy yet looks exactly like a defect and is not one.
             */
            bw_sync ();
            usleep (25000);
            bw_where (win, &px, &py, &w, &h);
        }

        /* The pattern, at this step's offset, over the whole new size */
        start = -(offset % BAND);
        for (y = start, j = 0; y < h; y += BAND, j++)
        {
            bw_fill (win, palette[(offset / BAND + j) % NCOL], 0, y, w, BAND);
        }
        bw_present (win);
        usleep (9000);
        bw_pump ();

        /*
         * A region inside the smallest size the window ever takes, so it is
         * inside the window whichever size is currently on screen. That is
         * what makes this usable while the compositor lags the server under
         * load: there is no moment when the captured area is not the window.
         * Capturing the full current size instead reads as a defect every time
         * the compositor is one frame behind, which under load is 20% of
         * resizes. Nothing tells a manager how big the window has to be, so in
         * managed mode the smallest size is whatever it granted, not BASEW.
         */
        iw = (w < BASEW) ? w : BASEW;
        ih = (h < BASEH) ? h : BASEH;
        img = NULL;
        if (iw > 2 * INSET && ih > 2 * INSET)
        {
            img = bw_capture (px + INSET, py + INSET,
                              iw - 2 * INSET, ih - 2 * INSET);
        }
        if (img == NULL)
        {
            /* Warmup is unscored, and the pattern must keep moving even past
               a failed capture or the next step repeats this offset */
            if (s >= 0)
            {
                no_capture++;
            }
            offset += BAND / 4;
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

                    if (colour_index (bw_pixel (img, cx, y)) !=
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
            bw_image *edge = bw_capture (px, py, w, h);

            if (edge != NULL)
            {
                int fx, fy, bad = 0;
                int minx = edge->width, maxx = -1;
                int miny = edge->height, maxy = -1;

                for (fy = 2; fy < edge->height - 2; fy += 3)
                {
                    for (fx = 2; fx < edge->width - 2; fx += 3)
                    {
                        if (colour_index (bw_pixel (edge, fx, fy)) < 0)
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
                        covered = 1;
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
                bw_image_free (edge);
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
                if (colour_index (bw_pixel (img, img->width / 2, y)) >= 0)
                {
                    known++;
                }
            }
            if (known == 0)
            {
                /*
                 * Not one row of ours anywhere: we are photographing another
                 * window, not a defect in this one. See the same reasoning at
                 * the edge test above. The edge test looks at the same step
                 * and may have said so already, and one step covered is one
                 * step that proved nothing, not two.
                 */
                if (!covered)
                {
                    blind++;
                    printf ("step %d: something is covering the window, "
                            "nothing proved\n", s);
                }
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
        bw_image_free (img);
        offset += BAND / 4;
    }

    bw_destroy (win);
    bw_close ();

    printf ("resizes: %d coherent, %d mixed, %d not the pattern, "
            "%d with a band at an edge, %d proved nothing, "
            "%d never photographed\n",
            clean, incoherent, foreign, edge_bad, blind, no_capture);

    if (incoherent > 0 || foreign > 0 || edge_bad > 0)
    {
        return 1;
    }
    /* Covered throughout, so every step was somebody else's pixels: no answer
       either way, which is not the same as a fault. See the README. Nothing
       scored and no picture taken is not an answer either: the screen could
       not be photographed at all, which is 2 and not a fault of anybody's. */
    if (clean == 0)
    {
        if (blind > 0)
        {
            return 3;
        }

        return (no_capture > 0) ? 2 : 1;
    }

    return 0;
}
