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
#include "stage.h"

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
 * Move (and size) a window. The pager request _NET_MOVERESIZE_WINDOW is tried
 * first because some compositors quietly ignore a plain client move of a
 * mapped window (mutter on Wayland does); others ignore the pager request
 * instead (labwc), and there the plain call is used. probe_moves() decides
 * which, once, by looking at where the window actually went.
 */
static void put_window (Display *d, Window root, Window win,
                        int x, int y, int w, int h)
{
    XClientMessageEvent mev;

    if (plain_moves)
    {
        if (w > 0)
        {
            XMoveResizeWindow (d, win, x, y, (unsigned) w, (unsigned) h);
        }
        else
        {
            XMoveWindow (d, win, x, y);
        }

        return;
    }

    memset (&mev, 0, sizeof mev);
    mev.type = ClientMessage;
    mev.window = win;
    mev.message_type = XInternAtom (d, "_NET_MOVERESIZE_WINDOW", False);
    mev.format = 32;
    mev.data.l[1] = x;
    mev.data.l[2] = y;
    if (w > 0)
    {
        mev.data.l[0] = 10 | (15 << 8) | (2 << 12);
        mev.data.l[3] = w;
        mev.data.l[4] = h;
    }
    else
    {
        mev.data.l[0] = 10 | (3 << 8) | (2 << 12);
    }
    XSendEvent (d, root, False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &mev);
}

static void probe_moves (Display *d, Window root, Window win,
                         int x, int y, int w, int h)
{
    int px, py;
    Window ch;

    put_window (d, root, win, x, y, w, h);
    XSync (d, False);
    usleep (400000);
    XTranslateCoordinates (d, win, root, 0, 0, &px, &py, &ch);
    if (px < x - 80 || px > x + 80 || py < y - 80 || py > y + 80)
    {
        plain_moves = 1;
        put_window (d, root, win, x, y, w, h);
        XSync (d, False);
        usleep (300000);
    }
}

static double now (void)
{
    struct timespec ts;

    clock_gettime (CLOCK_MONOTONIC, &ts);

    return ts.tv_sec + ts.tv_nsec / 1e9;
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
        XFillRectangle (d, win, gc, (i * 53) % (winw - 120),
                        (i * 71) % (winh - 90), 120, 90);
    }
    XSync (d, False);

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 60 : 0;        /* the first moves are not counted */
    probe_moves (d, root, win, cx, base_y, winw, winh);

    start = now ();
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

        if (tasks > 0)
        {
            if (i + 1 == warm)
            {
                mark ("MEASURE-START");
                mstart = now ();
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
        else if (now () - start >= seconds)
        {
            break;
        }

        /*
         * Held to a fixed rate, so every renderer is given exactly the same
         * amount of moving to composite and their cost can be compared. Left
         * free it just measures how fast this program can spam the server,
         * which came out at 89000 steps a second and composited none of them.
         */
        while (now () < due)
        {
            usleep (200);
        }
    }

    if (tasks > 0)
    {
        mark ("MEASURE-END");
        printf ("AVERAGE %.1f steps/s over %.1f s, %ld steps\n",
                steps / (now () - mstart), now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.1f steps/s over %.0f s\n", steps / (now () - start),
                seconds);
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    return 0;
}
