/*
 *   argbbench <seconds> [steps per second] [margin]
 *
 * BENCH_TASKS=N does N steps instead of running for the seconds given.
 *
 * A client-side-decorated window, which is what every modern toolkit makes:
 * 32-bit ARGB pixels, full window opacity, and an opaque region declaring
 * everything except a margin for its own rounded corners and shadow. Only that
 * margin needs blending, and a compositor that blends the whole window every
 * frame reads the destination as well as the texture for nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gate.h"
#include "now.h"
#include "win.h"
#include "place.h"

#define WINW 1600
#define WINH 1000

/*
 * Fixed work: BENCH_TASKS=N does exactly N tasks, however long that takes, so
 * every session performs the same amount of work and the numbers compare. The
 * measured part is bracketed by the two marks, after an unmeasured warm-up.
 */
static long bench_tasks (void)
{
    const char *e = getenv ("BENCH_TASKS");

    return (e != NULL && *e != '\0') ? atol (e) : 0;
}

static void mark (const char *s)
{
    if (strcmp (s, "MEASURE-START") == 0)
    {
        /* In a mix of programs, wait until the whole load is up. See gate.c */
        bench_wait_go ();
    }
    printf ("%s\n", s);
    fflush (stdout);
}

int main (int argc, char **argv)
{
    bw_win *win;
    double seconds = (argc > 1) ? atof (argv[1]) : 12.0;
    double rate = (argc > 2) ? atof (argv[2]) : 120.0;

    if (rate <= 0.0)
    {
        rate = 120.0;
    }
    int margin = (argc > 3) ? atoi (argv[3]) : 40;

    /*
     * The redraw below moves a 400x300 patch inside the margin and takes its
     * position modulo what is left over: a margin that leaves nothing over is
     * a divide by zero on the first step, and one past half the window turns
     * the opaque region into a huge unsigned size.
     */
    if (margin < 0 || 2 * margin >= WINW - 400 || 2 * margin >= WINH - 300)
    {
        margin = 40;
    }
    int i, j, steps = 0, warm;
    long tasks, done = 0;
    double start, mstart;
    unsigned long colours[6] = {
        0xffc04040, 0xff40c040, 0xff4040c0, 0xffc0c040, 0xffc040c0, 0xff40c0c0
    };

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    /* 32-bit pixels with alpha, the way a toolkit doing its own decorations
       asks */
    win = bw_create (NULL, 120, 120, WINW, WINH, "argbbench",
                     BW_PLACED | BW_ARGB | BW_UNMANAGED);
    if (win == NULL)
    {
        fprintf (stderr, "no 32-bit visual\n");

        return 2;
    }

    /*
     * Everything but the margin is opaque, which is exactly what GTK says.
     * The rectangle is relative to the client window.
     */
    bw_opaque_region (win, margin, margin, WINW - 2 * margin, WINH - 2 * margin);

    bw_map (win);
    bw_sync ();
    sleep (3);
    bench_placed (win, 120, 120, "argbbench");

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 10 : 0;
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;

        /* Redraw inside the opaque part, the way an application's content does */
        for (j = 0; j < 4; j++)
        {
            bw_fill (win, colours[(i + j) % 6],
                     margin + ((i * 37 + j * 211) % (WINW - 2 * margin - 400)),
                     margin + ((i * 53 + j * 97) % (WINH - 2 * margin - 300)),
                     400, 300);
        }
        bw_present (win);
        bw_sync ();
        steps++;

        if (tasks > 0)
        {
            if (i + 1 == warm)
            {
                mark ("MEASURE-START");
                mstart = bench_now ();
                /*
                 * The gate in mark() can have held this a long time, and the
                 * schedule counts from start: left alone it would run flat out
                 * to catch up, which is not the fixed rate this load is for.
                 */
                start = mstart - (double) (i + 1) / rate;
                steps = 0;
            }
            else if (i + 1 > warm)
            {
                done++;
            }
            if (done >= tasks)
            {
                break;
            }
        }
        else if (bench_now () - start >= seconds)
        {
            break;
        }

        bench_wait_until (due);
    }

    if (tasks > 0)
    {
        mark ("MEASURE-END");
        printf ("AVERAGE %.1f steps/s over %.1f s, %ld steps\n",
                steps / (bench_now () - mstart), bench_now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.1f steps/s over %.0f s\n",
                steps / (bench_now () - start), seconds);
    }

    bw_destroy (win);
    bw_close ();

    return 0;
}
