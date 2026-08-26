/*
 *   iconify_check [rounds]
 *
 * A different path from a menu appearing: the window goes on existing while
 * unmapped, so the compositor frees what it had built for it and has to build
 * it again for a window it already knows about.
 *
 * The minimise is proved rather than assumed - afterwards the pattern must be
 * gone from where it was, and a round where it is still there is reported as
 * having proved nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win.h"

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

static int pattern_score (int ox, int oy, int *total)
{
    bw_image *img;
    int x, y, hit = 0, n = 0;

    img = bw_capture (ox + MARGIN, oy + MARGIN,
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
            if (colour_index (bw_pixel (img, cx, y)) == band_of (y + MARGIN))
            {
                hit++;
            }
        }
    }
    bw_image_free (img);
    *total = n;

    return hit;
}

int main (int argc, char **argv)
{
    bw_win *win;
    int rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, y, ox, oy, ok = 0, bad = 0, inconclusive = 0, nocapture = 0;

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    if (bw_is_wayland ())
    {
        /* Fullscreen, so the origin is known without asking anyone */
        win = bw_create (NULL, 0, 0, WINW, WINH, "iconify_check", BW_NOTIFY);
        if (win != NULL)
        {
            bw_fullscreen (win, 1);
        }
    }
    else
    {
        win = bw_create (NULL, 150, 150, WINW, WINH, "iconify_check",
                         BW_PLACED | BW_NOTIFY);
    }
    if (win == NULL)
    {
        fprintf (stderr, "no window\n");

        return 2;
    }
    bw_map (win);
    bw_sync ();
    sleep (3);

    bw_where (win, &ox, &oy, NULL, NULL);

    for (r = 0; r < rounds; r++)
    {
        int hit, total, gone_hit, gone_total;

        for (y = 0; y < WINH; y += BAND)
        {
            bw_fill (win, palette[band_of (y)], 0, y, WINW, BAND);
        }
        bw_present (win);
        bw_sync ();
        usleep (500000);

        hit = pattern_score (ox, oy, &total);
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

        bw_minimize (win);
        bw_sync ();
        usleep (900000);

        gone_hit = pattern_score (ox, oy, &gone_total);
        if (gone_total == 0)
        {
            printf ("round %d: the screen cannot be photographed after "
                    "minimising\n", r + 1);
            nocapture++;
            bw_restore (win);
            bw_sync ();
            usleep (700000);
            continue;
        }
        if (gone_hit == gone_total)
        {
            printf ("round %d: still fully there after minimising, so the "
                    "minimise did not happen and this proves nothing\n", r + 1);
            inconclusive++;
            /* Put it back anyway before the next round */
            bw_restore (win);
            bw_sync ();
            usleep (700000);
            continue;
        }

        /* Restore, then redraw: unmapped windows are not kept for us */
        bw_restore (win);
        bw_sync ();
        usleep (900000);
        for (y = 0; y < WINH; y += BAND)
        {
            bw_fill (win, palette[band_of (y)], 0, y, WINW, BAND);
        }
        bw_present (win);
        bw_sync ();
        usleep (700000);

        /* The manager may have put it somewhere else on restoring */
        bw_where (win, &ox, &oy, NULL, NULL);
        hit = pattern_score (ox, oy, &total);

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
        else if (bw_is_wayland () && !(bw_state (win) & BW_STATE_ACTIVE) &&
                 hit * gone_total <= gone_hit * total)
        {
            /*
             * The window never came back: it is no more on screen than it was
             * while minimised, and the compositor did not activate it either.
             * The state alone is not enough to say so - a compositor may
             * unminimise without handing over the focus, and then a genuinely
             * corrupted restore would be excused here as a refusal and the
             * check would stop catching the defect it exists for.
             */
            printf ("round %d: the restore was refused, so nothing can be "
                    "concluded\n", r + 1);
            inconclusive++;
        }
        else
        {
            bad++;
            printf ("round %d: after restoring, %d of %d sampled points are "
                    "wrong\n", r + 1, total - hit, total);
        }
    }

    bw_destroy (win);
    bw_close ();

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
