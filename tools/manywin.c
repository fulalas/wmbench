/*
 *   manywin <count> [seconds]
 *
 * Many windows sitting there doing nothing while one small thing animates,
 * which is what a real desktop looks like. With no seconds given, or zero, it
 * holds them until it is killed: the windows are scenery, not work, so there is
 * nothing to count here and nothing that should end a measurement.
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
#define OVER 25                 /* how far each extra bank is offset */

/*
 * A window covered by its own offset twin shows only its leftmost OVER pixels,
 * and two windows drawn from the same pattern cannot be told apart there. The
 * corner carries a colour of its own so a screenshot can name every window.
 */
static unsigned long tag_colour (int i)
{
    return ((unsigned long) (40 + (i % 5) * 50) << 16) |
           ((unsigned long) (40 + ((i / 5) % 5) * 50) << 8) |
           (unsigned long) (40 + ((i / 25) % 5) * 50);
}

int main (int argc, char **argv)
{
    bw_win **wins, **pat;
    int count = (argc > 1) ? atoi (argv[1]) : 20;
    double seconds = (argc > 2) ? atof (argv[2]) : 0.0;
    int i, j, cols, sw, sh, sx, sy, lastx = 0, lasty = 0;
    unsigned long colours[6] = {
        0x904040, 0x409040, 0x404090, 0x909040, 0x904090, 0x409090
    };

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    bw_stage (STAGE_MARGIN, &sx, &sy, &sw, &sh);
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
    pat = calloc (count, sizeof (bw_win *));
    if (wins == NULL || pat == NULL)
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
     * wherever nothing composites and keeps the contents for us. One pattern
     * per window, not one per colour: windows sharing a pattern cannot be told
     * from each other in a screenshot.
     */
    for (i = 0; i < count; i++)
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
            bw_fill (pat[i], colours[(i + j) % 6],
                     (j * 31 + i * 57) % (WINW - 70),
                     (j * 43 + i * 89) % (WINH - 50), 70, 50);
        }
        bw_fill (pat[i], tag_colour (i), 0, 0, OVER, OVER);
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
        x += OVER * (i / (cols * rows));

        wins[i] = bw_create (NULL, x, y, WINW, WINH, "manywin",
                             BW_PLACED | BW_UNMANAGED);
        if (wins[i] == NULL)
        {
            fprintf (stderr, "no window\n");

            return 2;
        }
        bw_set_background (wins[i], pat[i]);
        bw_map (wins[i]);
        lastx = x;
        lasty = y;
    }
    bw_sync ();
    sleep (3);
    /*
     * The last window opened, not the first: a window an offset bank covers
     * shows mostly its neighbour's pixels, and the proof read that as a window
     * placed somewhere else. It only passed while every bank was drawn from
     * the same six patterns, which made the neighbour's pixels a match.
     */
    bench_placed (wins[count - 1], lastx, lasty, "manywin");

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
    for (i = 0; i < count; i++)
    {
        bw_destroy (pat[i]);
    }
    bw_close ();
    free (wins);
    free (pat);

    return 0;
}
