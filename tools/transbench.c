/*
 * Translucency. Nothing else here asks the compositor to blend a whole
 * managed window uniformly: argbbench hands it per-pixel alpha and popbench
 * hands it shadows. Yet a translucent terminal over a busy window is one of
 * the commonest things on a real desktop, and it is the case where the
 * compositor cannot just copy: it has to read what is underneath and blend.
 *
 * A detailed opaque window sits underneath. A second window on top carries
 * the opacity - _NET_WM_WINDOW_OPACITY on X11, wp-alpha-modifier on Wayland -
 * and redraws at a fixed rate, so every step forces the compositor to repaint
 * the background there and blend the top window over it.
 *
 *   transbench <seconds> [opacity 0..1] [steps per second]
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

#define BGW 1700
#define BGH 1050
#define FGW 900
#define FGH 600

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

/*
 * The gate is not folded into mark() here, the way the other loads have it:
 * on Wayland the translucent window is mapped between the two, so they have
 * to be called separately. See the MEASURE-START site below.
 */
static void mark (const char *s)
{
    printf ("%s\n", s);
    fflush (stdout);
}

int main (int argc, char **argv)
{
    bw_win *bg, *fg;
    double seconds = (argc > 1) ? atof (argv[1]) : 12.0;
    double alpha = (argc > 2) ? atof (argv[2]) : 0.75;
    double rate = (argc > 3) ? atof (argv[3]) : 120.0;

    if (alpha < 0.0)
    {
        alpha = 0.0;
    }
    if (alpha > 1.0)
    {
        alpha = 1.0;
    }
    if (rate <= 0.0)
    {
        rate = 120.0;
    }
    int i, steps = 0;
    int bgw, bgh, fgw, fgh, sx, sy, fg_up = 0;
    long tasks, warm, done = 0;
    double start, mstart;
    unsigned long colours[6] = {
        0xc04040, 0x40c040, 0x4040c0, 0xc0c040, 0xc040c0, 0x40c0c0
    };

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }

    bw_stage (STAGE_MARGIN, &sx, &sy, &bgw, &bgh);
    /* Two strips in from the stage edge: popbench's background takes the
       first and fsbench, under both, keeps the last. See place.h */
    bgw = MIN (BGW, bgw - 2 * STAGE_STRIP); bgh = MIN (BGH, bgh);
    if (bgw < STAGE_MINW)
    {
        bgw = STAGE_MINW;
    }
    fgw = MIN (FGW, bgw - 220); fgh = MIN (FGH, bgh - 220);
    bg = bw_create (NULL, sx, sy, bgw, bgh, "transbench background",
                    BW_PLACED | BW_UNMANAGED);
    fg = bw_create (NULL, sx + 220, sy + 220, fgw, fgh,
                    "transbench translucent", BW_PLACED | BW_UNMANAGED);
    if (bg == NULL || fg == NULL)
    {
        fprintf (stderr, "no window\n");

        return 2;
    }

    if (!bw_opacity (fg, alpha))
    {
        /* Without the protocol the window would simply be opaque, and the
           number would read as a result for blending that never happened */
        printf ("TRANS-REFUSED this session does not set window opacity\n");
        fflush (stdout);

        return 3;
    }

    bw_map (bg);
    if (!bw_is_wayland ())
    {
        /* Mapped now and raised politely below, the X11 way */
        bw_map (fg);
        fg_up = 1;
    }
    bw_sync ();
    sleep (3);
    if (fg_up)
    {
        bw_raise (fg);
        /* Politely, or GNOME posts a notification instead of raising */
        bw_activate (fg);
        bw_sync ();
        sleep (1);
    }
    bench_placed (bg, sx, sy, "transbench background");
    bench_placed (fg, sx + 220, sy + 220, "transbench translucent");

    /* Detail underneath, so the blend has something to read */
    for (i = 0; i < 400; i++)
    {
        bw_fill (bg, colours[i % 6], (i * 37) % (bgw - 90), (i * 61) % (bgh - 70),
                 90, 70);
    }
    bw_present (bg);
    bw_sync ();

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 60 : 0;
    /*
     * On Wayland nothing restacks another program's windows, so the
     * translucent one maps at the starting gun instead: the last window up
     * lands on top, which is the place the X11 run raises it to.
     */
    if (!fg_up && tasks == 0)
    {
        bw_map (fg);
        bw_sync ();
        fg_up = 1;
    }
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;

        bw_fill (fg, colours[i % 6], (i * 23) % (fgw - 200),
                 (i * 29) % (fgh - 150), 200, 150);
        bw_present (fg);
        bw_sync ();
        steps++;

        if (tasks > 0)
        {
            if (i + 1 == warm)
            {
                /*
                 * The gate, then the window, then the marker - in that order.
                 * The gate so the translucent window maps at the starting gun
                 * and lands on top. The marker last because the scripts start
                 * the clock, the compositor's CPU baseline and the power
                 * sampling the moment they see it, and the compositor's cost
                 * of creating this surface and answering its configure is not
                 * work the X11 run pays for - there the window has been up
                 * since long before. See gate.c.
                 */
                bench_wait_go ();
                if (!fg_up)
                {
                    bw_map (fg);
                    bw_sync ();
                    fg_up = 1;
                }
                mark ("MEASURE-START");
                mstart = bench_now ();
                /*
                 * The gate in mark() can have held this for a long time, and
                 * the schedule below counts from start: left alone it would
                 * now run flat out to catch up, which is not the fixed rate
                 * this load is supposed to make. Put it back on the clock.
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
        printf ("AVERAGE %.1f steps/s over %.0f s\n", steps / (bench_now () - start),
                seconds);
    }

    bw_destroy (fg);
    bw_destroy (bg);
    bw_close ();

    return 0;
}
