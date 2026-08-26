/*
 * A desktop with a lot of windows on it, which nothing here has covered. The
 * eight-window stress had eight windows all drawing flat out, measuring
 * throughput; this is the other shape of the same question: many windows
 * sitting there doing nothing while one small thing animates, which is what a
 * real desktop looks like.
 *
 * It is what decides whether skipping the blended pass for windows whose
 * shadow is nowhere near the damage is worth its four rectangle tests.
 *
 *   manywin <count> [seconds]
 *
 * Opens <count> ordinary managed windows with content, spread over the screen,
 * and holds them there. With no seconds given, or zero, it holds them until it
 * is killed: the windows are scenery, not work, so there is nothing to count
 * here and nothing that should end a measurement. Whoever started it decides
 * when the screen is no longer needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "win.h"
#include "now.h"
#include "place.h"

#define WINW 420
#define WINH 320

int main (int argc, char **argv)
{
    bw_win **wins;
    bw_win *pat[6];
    int count = (argc > 1) ? atoi (argv[1]) : 20;
    double seconds = (argc > 2) ? atof (argv[2]) : 0.0;
    int i, j, cols, sw, sh, sx, sy;
    unsigned long colours[6] = {
        0x904040, 0x409040, 0x404090, 0x909040, 0x904090, 0x409090
    };

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    bw_stage (60, &sx, &sy, &sw, &sh);
    cols = sw / (WINW + 40);
    if (cols < 1)
    {
        cols = 1;
    }

    if (count < 1)
    {
        fprintf (stderr, "count must be at least 1\n");

        return 2;
    }
    wins = calloc (count, sizeof (bw_win *));
    if (wins == NULL)
    {
        fprintf (stderr, "out of memory\n");

        return 2;
    }

    /*
     * The content is the windows' background rather than something painted
     * once after mapping. Nothing here listens for damage, and these windows
     * are scenery other benchmarks move over - stress_start() walks two
     * windows across them - so a strip that is uncovered gets filled from the
     * background: painted once it would come back black and stay black,
     * wherever nothing composites and keeps the contents for us. The pattern
     * only depends on i % 6, so six of them cover any number of windows.
     */
    for (i = 0; i < 6; i++)
    {
        pat[i] = bw_canvas (WINW, WINH);
        if (pat[i] == NULL)
        {
            fprintf (stderr, "out of memory\n");

            return 2;
        }
        bw_fill (pat[i], 0x000000, 0, 0, WINW, WINH);
        for (j = 0; j < 24; j++)
        {
            bw_fill (pat[i], colours[(i + j) % 6], (j * 31) % (WINW - 70),
                     (j * 43) % (WINH - 50), 70, 50);
        }
    }

    for (i = 0; i < count; i++)
    {
        /* However many fit; the rest start again from the top, offset */
        int rows = sh / (WINH + 60);
        int x, y;

        if (rows < 1)
        {
            rows = 1;
        }
        x = sx + (i % cols) * (WINW + 40);
        y = sy + ((i / cols) % rows) * (WINH + 60);
        x += 25 * (i / (cols * rows));

        wins[i] = bw_create (NULL, x, y, WINW, WINH, "manywin",
                             BW_PLACED | BW_UNMANAGED);
        if (wins[i] == NULL)
        {
            fprintf (stderr, "no window\n");

            return 2;
        }
        bw_set_background (wins[i], pat[i % 6]);
        bw_map (wins[i]);
    }
    bw_sync ();
    sleep (3);
    bench_placed (wins[0], sx, sy, "manywin");

    printf ("READY %d windows\n", count);
    fflush (stdout);
    if (bw_is_wayland ())
    {
        /* The connection has to stay fed - a ping left unanswered is a
           disconnect - so the scenery wakes now and then instead of pausing */
        double end = bench_now () + seconds;

        while (seconds <= 0.0 || bench_now () < end)
        {
            bw_pump ();
            usleep (200000);
        }
    }
    else if (seconds > 0.0)
    {
        usleep ((useconds_t) (seconds * 1e6));
    }
    else
    {
        for (;;)
        {
            pause ();
        }
    }

    for (i = 0; i < count; i++)
    {
        bw_destroy (wins[i]);
    }
    for (i = 0; i < 6; i++)
    {
        bw_destroy (pat[i]);
    }
    bw_close ();
    free (wins);

    return 0;
}
