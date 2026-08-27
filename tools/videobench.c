/*
 *   videobench <seconds> [frames per second] [width] [height]
 *
 * BENCH_TASKS=N hands over N frames instead of running for the seconds given.
 *
 * What makes this different from the other loads is where the pixels come
 * from: a player hands over a whole buffer it filled with the CPU, which may
 * not be laid out the way a GPU-rendered one is, and sampling it can be slow.
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
    double rate = (argc > 2) ? atof (argv[2]) : 60.0;

    if (rate <= 0.0)
    {
        rate = 60.0;
    }
    int w = (argc > 3) ? atoi (argv[3]) : 1920;
    int h = (argc > 4) ? atoi (argv[4]) : 1080;

    /* The frame loop writes four pixels at a time */
    w &= ~3;
    int i, x, y, frames = 0, warm, stride;
    long tasks, done = 0;
    double start, mstart;

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    win = bw_create (NULL, 100, 100, w, h, "videobench",
                     BW_PLACED | BW_UNMANAGED);
    if (win == NULL)
    {
        fprintf (stderr, "no window\n");

        return 2;
    }
    bw_map (win);
    bw_sync ();
    sleep (2);
    bench_placed (win, 100, 100, "videobench");

    if (bw_frame_pixels (win, &stride) == NULL)
    {
        fprintf (stderr, "no way to hand whole frames over on this session\n");

        return 2;
    }
    /* A loop that filled only the asked-for corner of a HiDPI window would
       hand the compositor a frame of undefined pixels */
    bw_frame_size (win, &w, &h);
    w &= ~3;

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 10 : 0;
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;
        /* Asked for again each frame: the Wayland backend rotates buffers */
        char *data = bw_frame_pixels (win, &stride);

        if (data == NULL)
        {
            fprintf (stderr, "the session took the frame buffer away\n");

            return 2;
        }
        /* A cheap moving pattern, the way a decoder hands over a new frame */
        for (y = 0; y < h; y++)
        {
            unsigned int *line = (unsigned int *)
                (data + (size_t) y * stride);
            unsigned int v = (unsigned int) (((y + i * 3) & 0xff) << 8);

            for (x = 0; x < w; x += 4)
            {
                line[x] = v | (unsigned) ((x + i) & 0xff);
                line[x + 1] = v;
                line[x + 2] = v | 0x400000;
                line[x + 3] = v;
            }
        }
        bw_frame_push (win);
        bw_sync ();
        frames++;

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
                frames = 0;
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
        printf ("AVERAGE %.1f frames/s over %.1f s, %ld frames\n",
                frames / (bench_now () - mstart), bench_now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.1f frames/s over %.0f s\n",
                frames / (bench_now () - start), seconds);
    }

    bw_destroy (win);
    bw_close ();

    return 0;
}
