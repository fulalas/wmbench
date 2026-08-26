/*
 *   popbench <seconds> [cycles per second] [popups] [width] [height]
 *
 * The one case where the per-window setup cost dominates rather than the
 * per-frame drawing: a compositor has to build a texture and a binding for
 * every window that appears.
 *
 * The popups are what real menus are - override-redirect windows on X11,
 * xdg_popups of the background window on Wayland - mapped and unmapped in turn
 * at a fixed rate, so every session is given identical work. A small height
 * matters: shadow drawing often takes a slower path below twice the blur
 * radius, and tooltips are short and wide.
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

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

#define BGW  1600
#define BGH  1000
static int popw = 340;
static int poph = 440;

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
    bw_win *bg, **pop;
    double seconds = (argc > 1) ? atof (argv[1]) : 12.0;
    double rate = (argc > 2) ? atof (argv[2]) : 20.0;

    if (rate <= 0.0)
    {
        rate = 20.0;
    }
    int npop = (argc > 3) ? atoi (argv[3]) : 6;
    int i, j, cycles = 0;
    int bgw, bgh, bgx, bgy, maxx, maxy, fillx, filly;
    int sx, sy, sh, fx, fy, fw, fh;
    long tasks, warm, done = 0;
    double mstart;

    if (argc > 4) popw = atoi (argv[4]);
    if (argc > 5) poph = atoi (argv[5]);
    if (popw < 40) popw = 40;
    if (poph < 40) poph = 40;
    double start;
    unsigned long colours[6] = {
        0xc04040, 0x40c040, 0x4040c0, 0xc0c040, 0xc040c0, 0x40c0c0
    };

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    bw_stage (80, &sx, &sy, NULL, &sh);
    bw_stage (STAGE_MARGIN, &fx, &fy, &fw, &fh);
    /*
     * Right edges in steps of STRIP, counted from the frame every load shares
     * rather than from this one: fsbench under everything ends at the edge,
     * this window one strip in, and transbench's wider background - which
     * opens above this one - another strip in again. Sized to the step and not
     * merely shifted to it, or on a screen too narrow for the full width the
     * window slides back to the corner and transbench covers it whole.
     */
    bgh = MIN (BGH, sh);
    bgw = MIN (BGW, fx + fw - STAGE_STRIP - sx);
    if (bgw < STAGE_MINW)
    {
        bgw = STAGE_MINW;
    }
    bgx = fx + fw - STAGE_STRIP - bgw;
    bgy = sy;
    /* How far along and down the popups may march before starting again */
    maxx = bgw - 200 - popw;
    if (maxx < 1)
    {
        maxx = 1;
    }
    maxy = bgh - 200 - poph;
    if (maxy < 1)
    {
        maxy = 1;
    }
    bg = bw_create (NULL, bgx, bgy, bgw, bgh, "popbench background",
                    BW_PLACED | BW_UNMANAGED);
    if (bg == NULL)
    {
        fprintf (stderr, "no window\n");

        return 2;
    }
    bw_map (bg);
    bw_sync ();
    sleep (3);

    /*
     * Where the window really is, not where it was asked to be. The popups
     * below open relative to it, so placed from the asked-for corner they
     * open away from the window on any compositor that does its own placing,
     * and then the thing being measured is the desktop being redrawn under a
     * menu instead of this window.
     */
    bench_placed (bg, bgx, bgy, "popbench background");
    bw_where (bg, &bgx, &bgy, NULL, NULL);

    fillx = bgw - 80;
    if (fillx < 1)
    {
        fillx = 1;
    }
    filly = bgh - 60;
    if (filly < 1)
    {
        filly = 1;
    }
    for (i = 0; i < 300; i++)
    {
        bw_fill (bg, colours[i % 6], (i * 41) % fillx, (i * 67) % filly,
                 80, 60);
    }
    bw_present (bg);
    bw_sync ();

    /* What a menu really is: no frame, no manager, belongs to the window */
    if (npop < 1)
    {
        fprintf (stderr, "popups must be at least 1\n");

        return 2;
    }
    pop = calloc (npop, sizeof (bw_win *));
    if (pop == NULL)
    {
        fprintf (stderr, "out of memory\n");

        return 2;
    }
    for (i = 0; i < npop; i++)
    {
        /* Inside the stage, and inside the background window with it */
        pop[i] = bw_create (bg, bgx + 100 + (i * (popw + 30)) % maxx,
                            bgy + 100 + ((i % 3) * (poph + 40)) % maxy,
                            popw, poph, NULL, BW_POPUP);
        if (pop[i] == NULL)
        {
            fprintf (stderr, "no popup window\n");
            while (i-- > 0)
            {
                bw_destroy (pop[i]);
            }
            bw_destroy (bg);
            free (pop);

            return 2;
        }
    }
    bw_sync ();

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 10 : 0;
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;
        bw_win *w = pop[i % npop];

        bw_map (w);
        bw_raise (w);
        for (j = 0; j < 10; j++)
        {
            bw_fill (w, colours[(i + j) % 6], 10, 10 + j * 42, popw - 20, 36);
        }
        bw_present (w);
        bw_sync ();
        /*
         * At least one refresh period, so every popup is presented whatever
         * the phase. Shorter than a frame, and the 50 ms cycle - an exact
         * multiple of a 60 Hz frame - landed the map at the same point of
         * every frame: whole runs showed all the popups or none of them, by
         * where that point happened to fall.
         */
        usleep (25000);
        bw_unmap (w);
        bw_sync ();
        cycles++;

        if (tasks > 0)
        {
            if (i + 1 == warm)
            {
                mark ("MEASURE-START");
                mstart = bench_now ();
                /*
                 * The gate in mark() can have held this for a long time, and
                 * the schedule below counts from start: left alone it would
                 * now run flat out to catch up, which is not the fixed rate
                 * this load is supposed to make. Put it back on the clock.
                 */
                start = mstart - (double) (i + 1) / rate;
                cycles = 0;
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
        printf ("AVERAGE %.1f cycles/s over %.1f s, %ld cycles\n",
                cycles / (bench_now () - mstart), bench_now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.1f cycles/s over %.0f s\n",
                cycles / (bench_now () - start), seconds);
    }

    for (i = 0; i < npop; i++)
    {
        bw_destroy (pop[i]);
    }
    bw_destroy (bg);
    bw_close ();
    free (pop);

    return 0;
}
