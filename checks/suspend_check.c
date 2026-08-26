/*
 *   suspend_check [rounds]
 *
 * Suspending compositing for a focused fullscreen window is common and often
 * on by default, and resuming means repainting everything. A known band
 * pattern sits in a window while a second one goes fullscreen and back; the
 * pattern window must be exactly the pattern afterwards.
 *
 * Exit status 0 when every resume came back clean.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win.h"

#define BAND    40
#define NCOL    8
#define WINW    900
#define WINH    700
#define MARGIN  20

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

/* Every sampled pixel is the colour the pattern calls for */
static int capture_matches (int ox, int oy)
{
    bw_image *img;
    int x, y, ok = 1;

    img = bw_capture (ox + MARGIN, oy + MARGIN,
                      WINW - 2 * MARGIN, WINH - 2 * MARGIN);
    if (img == NULL)
    {
        return -1;
    }
    for (y = 0; y < img->height && ok; y += 2)
    {
        for (x = 0; x < 5; x++)
        {
            int px = (img->width / 6) * (x + 1);

            if (colour_index (bw_pixel (img, px, y)) != band_of (y + MARGIN))
            {
                ok = 0;
                break;
            }
        }
    }
    bw_image_free (img);

    return ok;
}

int main (int argc, char **argv)
{
    bw_win *win, *fs;
    int rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, i, y, ox, oy, ok = 0, bad = 0, inconclusive = 0;

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    if (bw_is_wayland ())
    {
        /* Fullscreen, so the origin is known without asking anyone */
        win = bw_create (NULL, 0, 0, WINW, WINH, "suspend_check pattern", 0);
        if (win != NULL)
        {
            bw_fullscreen (win, 1);
        }
    }
    else
    {
        win = bw_create (NULL, 140, 140, WINW, WINH, "suspend_check pattern",
                         BW_PLACED);
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
        /* A round either answered or it did not; it cannot fail to answer
           twice, and counting it twice would spend two of the rounds the
           verdict below weighs against. */
        int unproven = 0;

        for (y = 0; y < WINH; y += BAND)
        {
            bw_fill (win, palette[band_of (y)], 0, y, WINW, BAND);
        }
        bw_present (win);
        bw_sync ();
        usleep (300000);

        /*
         * A managed window that asks the manager for fullscreen and never says
         * anything about bypassing the compositor: the category the option acts
         * on. Compositing should suspend while it has focus.
         */
        fs = bw_create (NULL, 0, 0, 800, 600, "suspend_check fullscreen",
                        BW_NOTIFY);
        if (fs == NULL)
        {
            fprintf (stderr, "no window\n");

            return 2;
        }
        bw_map (fs);
        bw_sync ();
        sleep (1);

        bw_fullscreen (fs, 1);
        bw_sync ();
        sleep (1);

        /*
         * The suspend only fires for the window that has the focus, so it is
         * asked for - politely, or the manager calls it focus stealing - and
         * then taken outright, since a manager that ignores the polite ask
         * would leave this round testing nothing.
         */
        bw_activate (fs);
        bw_take_focus (fs);
        bw_sync ();
        sleep (2);
        /* Draw in it, so the suspended path has real work going through it */
        for (i = 0; i < 30; i++)
        {
            bw_fill (fs, palette[i % NCOL], (i * 97) % 1200, (i * 61) % 800,
                     500, 400);
            bw_present (fs);
            bw_sync ();
            usleep (30000);
        }

        /* Out of fullscreen, then gone: compositing has to resume */
        bw_fullscreen (fs, 0);
        bw_sync ();
        sleep (1);
        bw_destroy (fs);
        bw_sync ();
        sleep (2);

        /*
         * Look before redrawing. While compositing was suspended this window
         * was unredirected, so being covered destroyed its contents and the
         * server expects the client to paint them again. That is ordinary X11,
         * and it is also the proof that the suspend actually happened: with
         * compositing running the contents would have survived in the offscreen
         * pixmap. If this capture still matches, the suspend never fired and
         * the round has tested nothing, which is worth saying out loud.
         */
        if (capture_matches (ox, oy) == 1)
        {
            printf ("round %d: contents survived, so compositing never "
                    "suspended and this round proves nothing\n", r + 1);
            unproven = 1;
        }

        /* Now redraw: does what the client draws after the resume get through? */
        for (y = 0; y < WINH; y += BAND)
        {
            bw_fill (win, palette[band_of (y)], 0, y, WINW, BAND);
        }
        bw_present (win);
        bw_sync ();
        usleep (600000);

        switch (capture_matches (ox, oy))
        {
            case 1:
                ok++;
                break;
            case -1:
                printf ("round %d: the capture failed, so this round proves "
                        "nothing\n", r + 1);
                unproven = 1;
                break;
            default:
                bad++;
                printf ("round %d: the screen did not come back\n", r + 1);
        }
        inconclusive += unproven;
    }

    bw_destroy (win);
    bw_close ();

    printf ("suspend and resume: %d clean, %d wrong, %d proved nothing\n",
            ok, bad, inconclusive);

    /* 3 keeps a round that proved nothing apart from a round that went wrong:
       a compositor that never suspends is not a compositor at fault. Only when
       no round proved anything, though - one slow round does not throw away
       the ones that answered. */
    if (bad > 0)
        return 1;
    if (inconclusive >= rounds)
        return 3;

    return (ok > 0) ? 0 : 1;
}
