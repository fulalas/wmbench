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
#include "win.h"
#include "place.h"

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

static void draw_pattern (bw_win *w, int offset)
{
    int start = -(offset % BAND);
    int j = 0, y;

    for (y = start; y < WINH; y += BAND, j++)
    {
        bw_fill (w, palette[(offset / BAND + j) % NCOL], 0, y, WINW, BAND);
    }
    bw_present (w);
}

/* Every sampled pixel must be the colour this offset calls for */
static int matches (bw_image *img, int offset, int *bad_row)
{
    int x, y;

    for (y = 0; y < img->height; y += 2)
    {
        for (x = 0; x < 5; x++)
        {
            int px = (img->width / 6) * (x + 1);

            if (colour_index (bw_pixel (img, px, y)) !=
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
    bw_win *w1, *w2;
    bw_image *img;
    int rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, i, ox, oy, bad = 0;
    int settled_ok = 0, settled_bad = 0, stale_ok = 0, stale_bad = 0;

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    /*
     * Position and size are asked for and held: the window manager happily
     * puts the noise window on top of the pattern one otherwise, at which
     * point the capture is of the wrong window and everything looks stale.
     * Every capture below reads a fixed WINW x WINH region, so a window the
     * manager resized would be photographed along with whatever is beside it,
     * which reads as stale too.
     */
    w2 = bw_create (NULL, 100 + WINW + 120, 100, WINW, WINH,
                    "stale_check noise",
                    BW_PLACED | BW_FIXED | BW_AIMED | BW_UNMANAGED);
    w1 = bw_create (NULL, 100, 100, WINW, WINH,
                    "stale_check pattern",
                    BW_PLACED | BW_FIXED | BW_AIMED | BW_UNMANAGED);
    if (w1 == NULL || w2 == NULL)
    {
        fprintf (stderr, "no window\n");

        return 2;
    }
    if (!bench_aimable (w1))
    {
        return 3;
    }

    bw_map (w2);
    bw_map (w1);
    bw_sync ();
    sleep (3);
    /* The pattern window must be the visible one wherever they ended up */
    bw_raise (w1);
    bw_activate (w1);
    bw_sync ();
    sleep (1);

    bw_where (w1, &ox, &oy, NULL, NULL);
    {
        int x2, y2;

        bw_where (w2, &x2, &y2, NULL, NULL);
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
            draw_pattern (w1, offset);
            usleep (4000);
            bw_pump ();
            offset += STEP;
        }
        /* Land on a whole band so the expected picture is unambiguous */
        offset += BAND - (offset % BAND);
        draw_pattern (w1, offset);
        bw_sync ();
        usleep (600000);

        for (i = 0; i < 3; i++)
        {
            img = bw_capture (ox + MARGIN, oy + MARGIN,
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
            bw_image_free (img);
            usleep (150000);
        }

        /*
         * Stale. Nothing touches the pattern window now; the other window is
         * damaged as fast as it can be. If presenting only the damage leaves
         * anything behind, the pattern window is where it will show.
         */
        for (i = 0; i < 400; i++)
        {
            bw_fill (w2, palette[i % NCOL], (i * 37) % 700, (i * 53) % 500,
                     200, 200);
            if ((i % 40) == 0)
            {
                bw_present (w2);
                usleep (10000);
                bw_pump ();
            }
        }
        bw_present (w2);
        bw_sync ();
        usleep (400000);

        img = bw_capture (ox + MARGIN, oy + MARGIN,
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
        bw_image_free (img);
    }


    bw_destroy (w1);
    bw_destroy (w2);
    bw_close ();

    printf ("settled %d ok %d wrong, stale %d ok %d wrong\n",
            settled_ok, settled_bad, stale_ok, stale_bad);

    return (settled_bad == 0 && stale_bad == 0 &&
            settled_ok > 0 && stale_ok > 0) ? 0 : 1;
}
