/*
 * Moving and resizing a window, which is the compositing workload a person
 * actually watches. Everything measured so far has been a window sitting still
 * redrawing its inside; this one makes the window itself move, so the
 * compositor repaints the area it left as well as the area it arrived at, and
 * the window manager reconfigures a frame every step.
 *
 *   movebench <seconds> [move|resize] [steps per second]
 *
 * move walks a circle at a fixed size; resize stays put and changes size.
 *
 * Reports completed steps a second. Each step is synced, so the figure is how
 * fast the whole window manager, X server and compositor can carry a moving
 * window, not how fast this program can ask.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include "gate.h"
#include "now.h"
#include "stage.h"
#include "place.h"

#define WINW 1000               /* the size a big screen uses */
#define WINH 700
#define MARGIN 80                /* room for a frame and a panel */
#define GAP 40                   /* between the two windows of the mix */

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

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

static int plain_moves;         /* the pager request did nothing here */

/*
 * Move (and size) a window, the way bench_probe_move() found this session
 * honours. See lib/place.c: some compositors ignore a plain client move of a
 * mapped window (mutter on Wayland does), others ignore the pager request
 * (labwc), and one ignores both (cosmic-comp).
 */
static void put_window (Display *d, Window root, Window win,
                        int x, int y, int w, int h)
{
    bench_move (d, root, win, x, y, w, h, plain_moves);
}

int main (int argc, char **argv)
{
    Display *d;
    Window win, root;
    GC gc;
    Atom wtype, wtype_normal;
    double seconds = (argc > 1) ? atof (argv[1]) : 10.0;
    int resize = (argc > 2 && !strcmp (argv[2], "resize"));
    double rate = (argc > 3) ? atof (argv[3]) : 120.0;

    if (rate <= 0.0)
    {
        rate = 120.0;
    }
    int scr, i, steps = 0, base_y;
    int winw, winh, cx, cy, rx, ry, room_w, room_h, stage_x, stage_y;
    long tasks, warm, done = 0;
    double start, mstart;
    unsigned long colours[6] = {
        0xc04040, 0x40c040, 0x4040c0, 0xc0c040, 0xc040c0, 0x40c0c0
    };

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    /*
     * Lower down when resizing, so the two of these in the stress mix do not
     * sit on top of each other - but only where the screen has the room for
     * the biggest size this walks through, frame and all.
     */
    /*
     * The layout, worked out from the area the benchmark is allowed to use
     * rather than written down: the moving window walks its circle in the
     * upper band, and the resizing one sits in what is left below, where both
     * can be watched at once. Nothing here reaches outside that area on any
     * screen. See stage.c.
     */
    bench_stage (d, MARGIN, &stage_x, &stage_y, &room_w, &room_h);
    winw = MIN (WINW, room_w * 55 / 100);
    winh = MIN (WINH, room_h * 35 / 100);
    /* The circle keeps the whole window, at the widest it grows to, inside */
    rx = MIN (260, (room_w - (winw + 160)) / 2);
    ry = MIN (150, room_h * 8 / 100);
    if (rx < 0)
    {
        rx = 0;
    }
    cx = stage_x + rx;
    cy = stage_y + ry;

    base_y = cy;
    if (resize)
    {
        /* Below the circle, and no taller than the room left down there */
        int room = stage_y + room_h - (cy + ry + winh + GAP);

        if (room > 200)
        {
            base_y = cy + ry + winh + GAP;
            winh = MIN (winh, room - 110);
        }
    }

    win = XCreateSimpleWindow (d, root, cx, base_y, winw, winh, 0,
                               BlackPixel (d, scr), WhitePixel (d, scr));
    /*
     * Resizing gets its own name and its own place on the screen: the stress
     * mix runs one of each at the same time, and two windows called the same
     * thing walking the same circle could be neither told apart nor stacked in
     * a known order.
     */
    XStoreName (d, win, resize ? "movebench resize" : "movebench");
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    wtype_normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &wtype_normal, 1);
    XMapWindow (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (2);

    /* Something with detail in it, so the compositor has real pixels to move */
    for (i = 0; i < 60; i++)
    {
        XSetForeground (d, gc, colours[i % 6]);
        XFillRectangle (d, win, gc, (i * 53) % MAX (1, winw - 120),
                        (i * 71) % MAX (1, winh - 90), 120, 90);
    }
    XSync (d, False);

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 60 : 0;        /* the first moves are not counted */
    {
        int way = bench_probe_move (d, root, win, cx, base_y, winw, winh);

        plain_moves = (way == 1);
    }


    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        /*
         * The resizing window keeps a slower rhythm than the moving one. At
         * the same one they pulsed and swung in lockstep, which looks like
         * one animation in two places and reads as a glitch.
         */
        double a = i * (resize ? 0.037 : 0.09);
        double due = start + i / rate;
        /*
         * The whole window, frame included, stays on screen: WMs disagree
         * about a frame crossing the edge - some clamp it, some let it
         * leave - and either way the moving done would differ per WM. The
         * margin leaves room for any frame.
         */
        /*
         * Resizing stays where it is. Two windows walking the same circle in
         * the stress mix measured nothing the one moving window did not, and
         * it made the two of them impossible to tell apart on screen; a
         * window resized in place is also what a person actually does.
         */
        int x = resize ? cx : cx + (int) (rx * cos (a));
        int y = resize ? base_y : base_y + (int) (ry * sin (a));

        int nw = MAX (200, winw - 200 + (int) (180.0 * (1.0 + cos (a))));
        int nh = MAX (150, winh - 150 + (int) (130.0 * (1.0 + sin (a))));

        put_window (d, root, win, x, y, resize ? nw : 0, resize ? nh : 0);
        /* Wait for the server to have done it, so this counts real work */
        XSync (d, False);
        steps++;
        /*
         * Now and then, look at where the window really is. A compositor can
         * take every request and act on none of them, and then this row is a
         * measurement of a window sitting still - the smallest number in the
         * table, which reads as the best result in it.
         */
        if ((i % 64) == 0)
        {
            bench_watch (d, win);
        }

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

        /*
         * Held to a fixed rate, so every renderer is given exactly the same
         * amount of moving to composite and their cost can be compared. Left
         * free it just measures how fast this program can spam the server,
         * which came out at 89000 steps a second and composited none of them.
         */
        while (bench_now () < due)
        {
            usleep (200);
        }
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

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    /*
     * The window never went anywhere, so there is no moving to measure. Say
     * so with a status of its own: the row is left empty rather than filled
     * with the small number a compositor doing nothing produces, which reads
     * as the best result in the table.
     */
    if (!bench_moved () && !resize)
    {
        printf ("MOVE-NEVER-HAPPENED the window stayed where it was\n");
        fflush (stdout);

        return 3;
    }

    return 0;
}
