/*
 *   pop_check [rounds]
 *
 * Does what a menu covered come back correctly? The area a popup covered has
 * to be repainted from the window underneath when it unmaps. A known band
 * pattern sits in a window, popups are mapped over it and unmapped again many
 * times, and the pattern window must still be exactly the pattern.
 *
 * Exit status 0 when nothing was left behind.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "win.h"
#include "place.h"

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
    bw_win *win, *pop[3], *pat;
    bw_image *img;
    int rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, i, j, x, y, ox, oy, capw, caph, gw, gh, ok = 0, bad = 0;

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    win = bw_create (NULL, 120, 120, WINW, WINH, "pop_check pattern",
                     BW_PLACED | BW_AIMED | BW_UNMANAGED);
    if (win == NULL)
    {
        fprintf (stderr, "no window\n");

        return 2;
    }
    if (!bench_aimable (win))
    {
        return 3;
    }

    /*
     * The pattern is the window's background as well as what is drawn into it.
     * Where nothing composites, unmapping a popup makes the server clear the
     * area it covered to the background and send an Expose this program never
     * reads: with a plain colour that area stays blank until the next round
     * redraws it, which is after the capture, so a session behaving exactly as
     * X11 specifies would be accused of what the popup left behind. Filled
     * from the pattern the strip is right either way, and what is measured is
     * the compositor rather than the absence of one.
     */
    pat = bw_canvas (WINW, WINH);
    if (pat == NULL)
    {
        fprintf (stderr, "out of memory\n");

        return 2;
    }
    for (y = 0; y < WINH; y += BAND)
    {
        bw_fill (pat, palette[(y / BAND) % NCOL], 0, y, WINW, BAND);
    }
    bw_set_background (win, pat);
    bw_map (win);
    bw_sync ();
    sleep (3);

    bw_where (win, &ox, &oy, &gw, &gh);
    /*
     * And the size it really has. A manager is free to hand out a size of its
     * own - a tiling one sizes the window to its tile - so photographing the
     * size that was asked for would take in the desktop beside the window and
     * read every one of those pixels as something a popup left behind.
     */
    capw = (gw < WINW) ? gw : WINW;
    caph = (gh < WINH) ? gh : WINH;
    if (capw <= 2 * MARGIN || caph <= 2 * MARGIN)
    {
        fprintf (stderr, "the window is too small to photograph\n");

        return 2;
    }

    /* Popups land on top of the pattern, which is the whole point */
    for (i = 0; i < 3; i++)
    {
        pop[i] = bw_create (win, ox + 60 + i * 200, oy + 80 + i * 90,
                            POPW, POPH, NULL, BW_POPUP);
        if (pop[i] == NULL)
        {
            fprintf (stderr, "no popup window\n");

            return 2;
        }
    }
    bw_sync ();

    for (r = 0; r < rounds; r++)
    {
        /* Draw the pattern, then cover and uncover it repeatedly */
        bw_copy (pat, win, 0, 0, WINW, WINH, 0, 0);
        bw_present (win);
        bw_sync ();
        usleep (200000);

        for (i = 0; i < 24; i++)
        {
            bw_win *w = pop[i % 3];

            bw_map (w);
            bw_raise (w);
            for (j = 0; j < 6; j++)
            {
                bw_fill (w, palette[(i + j) % NCOL], 8, 8 + j * 46,
                         POPW - 16, 40);
            }
            bw_present (w);
            bw_sync ();
            usleep (25000);
            bw_unmap (w);
            bw_sync ();
            usleep (25000);
        }

        /* Everything is unmapped again; the pattern must be untouched */
        usleep (500000);
        img = bw_capture (ox + MARGIN, oy + MARGIN,
                          capw - 2 * MARGIN, caph - 2 * MARGIN);
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

                    if (colour_index (bw_pixel (img, px, y)) !=
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
        bw_image_free (img);
    }

    for (i = 0; i < 3; i++)
    {
        bw_destroy (pop[i]);
    }
    bw_destroy (pat);
    bw_destroy (win);
    bw_close ();

    printf ("popups: %d rounds clean, %d left something behind\n", ok, bad);

    return (bad == 0 && ok > 0) ? 0 : 1;
}
