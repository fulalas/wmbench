/*
 * Where a window actually ended up.
 *
 * Every load here asks for a place on the screen and then draws as if it got
 * it. Most window managers give it. Some place windows themselves and ignore
 * the request entirely - cosmic-comp does - and then a load meant to sit
 * beside another sits on top of it, menus open away from the window they
 * belong to, and the mix composites a scene nobody designed. The numbers
 * still come out, and they are numbers for a different picture.
 *
 * So: ask, look, and say so. A row whose scene was never built is worth less
 * than no row at all, because it reads as a result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include "place.h"

/* A frame and a title bar move a window down and right by their own size */
#define SLACK 100

void bench_where (Display *d, Window w, int *x, int *y, int *width, int *height)
{
    XWindowAttributes at;
    Window child;
    int rx = 0, ry = 0;

    if (!XGetWindowAttributes (d, w, &at))
    {
        at.width = at.height = 0;
    }
    XTranslateCoordinates (d, w, DefaultRootWindow (d), 0, 0, &rx, &ry, &child);
    if (x != NULL) *x = rx;
    if (y != NULL) *y = ry;
    if (width != NULL) *width = at.width;
    if (height != NULL) *height = at.height;
}

int bench_placed (Display *d, Window w, int ax, int ay, const char *what)
{
    int gx, gy, gw, gh, ok;

    XSync (d, False);
    bench_where (d, w, &gx, &gy, &gw, &gh);
    ok = (gx >= ax - SLACK && gx <= ax + SLACK &&
          gy >= ay - SLACK && gy <= ay + SLACK);
    printf ("place: %s asked %d,%d got %d,%d %dx%d\n",
            what, ax, ay, gx, gy, gw, gh);
    if (!ok)
    {
        printf ("PLACE-IGNORED %s\n", what);
    }
    fflush (stdout);

    return ok;
}

/*
 * The spread of everywhere a watched window has been seen. One box per
 * window: two windows sitting in different places are not a window that
 * moved, and sharing a box between them read as movement that never happened.
 */
#define WATCHED 4

static struct {
    Window w;
    int n, x0, y0, x1, y1;
} seen[WATCHED];

void bench_watch (Display *d, Window w)
{
    int x, y, i;

    for (i = 0; i < WATCHED; i++)
    {
        if (seen[i].n == 0 || seen[i].w == w)
        {
            break;
        }
    }
    if (i == WATCHED)
    {
        return;                 /* more windows than anyone watches */
    }
    bench_where (d, w, &x, &y, NULL, NULL);
    if (seen[i].n++ == 0)
    {
        seen[i].w = w;
        seen[i].x0 = seen[i].x1 = x;
        seen[i].y0 = seen[i].y1 = y;

        return;
    }
    if (x < seen[i].x0) seen[i].x0 = x;
    if (x > seen[i].x1) seen[i].x1 = x;
    if (y < seen[i].y0) seen[i].y0 = y;
    if (y > seen[i].y1) seen[i].y1 = y;
}

/* Far enough that no frame, shadow or rounding could account for it */
#define REALLY_MOVED 60

int bench_moved (void)
{
    int i, looked = 0;

    for (i = 0; i < WATCHED; i++)
    {
        if (seen[i].n < 2)
        {
            continue;
        }
        looked = 1;
        if ((seen[i].x1 - seen[i].x0) >= REALLY_MOVED ||
            (seen[i].y1 - seen[i].y0) >= REALLY_MOVED)
        {
            return 1;
        }
    }

    return !looked;             /* nobody looked, so nobody may complain */
}

/* The pager request, the one a taskbar sends */
static void pager_move (Display *d, Window root, Window w,
                        int x, int y, int width, int height)
{
    XClientMessageEvent mev;

    memset (&mev, 0, sizeof mev);
    mev.type = ClientMessage;
    mev.window = w;
    mev.message_type = XInternAtom (d, "_NET_MOVERESIZE_WINDOW", False);
    mev.format = 32;
    /* gravity 10 (static), x, y and - when asked for - width and height */
    mev.data.l[0] = (width > 0) ? (10 | (15 << 8) | (2 << 12))
                                : (10 | (3 << 8) | (2 << 12));
    mev.data.l[1] = x;
    mev.data.l[2] = y;
    mev.data.l[3] = width;
    mev.data.l[4] = height;
    XSendEvent (d, root, False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &mev);
}

void bench_move (Display *d, Window root, Window w,
                 int x, int y, int width, int height, int plain)
{
    if (plain)
    {
        if (width > 0)
        {
            XMoveResizeWindow (d, w, x, y, (unsigned) width, (unsigned) height);
        }
        else
        {
            XMoveWindow (d, w, x, y);
        }
    }
    else
    {
        pager_move (d, root, w, x, y, width, height);
    }
}

/*
 * Did the window move the way it was asked to? By how far it went, not by
 * where it ended up: a frame and a title bar offset every answer, and window
 * managers disagree about whether the coordinates in the request mean the
 * frame or the window inside it. The difference between before and after
 * cancels all of that out.
 */
#define STEP_X 120
#define STEP_Y 90
#define DRIFT   60

static int shifted (int dx, int dy)
{
    return dx >= STEP_X - DRIFT && dx <= STEP_X + DRIFT &&
           dy >= STEP_Y - DRIFT && dy <= STEP_Y + DRIFT;
}

/*
 * One attempt at one way of moving, waited for rather than slept through.
 * In the stress mix six programs put their windows up at the same moment and
 * the window manager answers a second late; a fixed sleep called that a
 * refusal and threw the row away. Polling returns as soon as the window has
 * gone, and only gives up when it really has not.
 */
static int try_move (Display *d, Window root, Window w, int plain,
                     const char *way)
{
    int bx, by, gx, gy, i;

    XSync (d, False);
    bench_where (d, w, &bx, &by, NULL, NULL);
    bench_move (d, root, w, bx + STEP_X, by + STEP_Y, 0, 0, plain);
    XSync (d, False);
    for (i = 0; i < 20; i++)    /* up to two seconds */
    {
        usleep (100000);
        bench_where (d, w, &gx, &gy, NULL, NULL);
        if (shifted (gx - bx, gy - by))
        {
            break;
        }
    }
    printf ("probe: %s moved %d,%d of %d,%d\n",
            way, gx - bx, gy - by, STEP_X, STEP_Y);
    fflush (stdout);

    return shifted (gx - bx, gy - by);
}

int bench_probe_move (Display *d, Window root, Window w,
                      int x, int y, int width, int height)
{
    int way = -1, tries;

    /*
     * Twice each way before giving up. Calling a session incapable of moving
     * a window is a heavy thing to say - the row it belongs to is dropped -
     * and one slow answer while the desktop is still settling is not proof.
     */
    for (tries = 0; tries < 3 && way < 0; tries++)
    {
        /*
         * The plain call first. Both ways work under most window managers,
         * and xfwm4 obeys the pager request only sometimes - picking whichever
         * answered first would send one run down one path in the window
         * manager and the next run down another, and the two numbers would
         * not be comparable. The plain call is the one a program makes, so it
         * is the one to prefer; the pager request is the fallback for the
         * compositors that ignore a plain move of a mapped window, which is
         * what mutter does on Wayland.
         */
        if (try_move (d, root, w, 1, "plain client calls"))
        {
            way = 1;
        }
        else if (try_move (d, root, w, 0, "the pager request"))
        {
            way = 0;
        }
    }

    /* Back where it belongs, by whichever way works */
    bench_move (d, root, w, x, y, width, height, (way == 1));
    XSync (d, False);
    usleep (300000);

    printf ("moves: %s\n", (way == 0) ? "the pager request"
                         : (way == 1) ? "plain client calls"
                                      : "neither way works");
    if (way < 0)
    {
        printf ("MOVE-REFUSED this session does not move a mapped window\n");
    }
    fflush (stdout);

    return way;
}
