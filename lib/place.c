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
 *
 * This file is the policy: what counts as moved, what the lines say. How a
 * window is moved and where it is believed to be is the backend's, and on
 * Wayland believing is not enough - the compositor never says where anything
 * is, so the answer is proved with a screenshot once before the measurement
 * and once after it, never inside it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win.h"
#include "now.h"
#include "place.h"

/* A frame and a title bar move a window down and right by their own size */
#define SLACK 100

static int screen_is_ours (void);

/*
 * Whether a believed position is proved with a screenshot. On by default; the
 * one caller that asks inside a measured phase turns it off, because the
 * screenshot costs about 0.3 s on Wayland and that would land in the numbers.
 */
static int prove_places = 1;

void bench_prove_places (int on)
{
    prove_places = on;
}

int bench_placed (bw_win *w, int ax, int ay, const char *what)
{
    int gx, gy, gw, gh, ok;

    bw_sync ();
    if (!bw_win_placed (w))
    {
        /* The session places this window itself and never says where */
        printf ("place: %s asked %d,%d got nowhere anyone can say\n",
                what, ax, ay);
        printf ("PLACE-IGNORED %s\n", what);
        fflush (stdout);

        return 0;
    }
    bw_where (w, &gx, &gy, &gw, &gh);
    ok = (gx >= ax - SLACK && gx <= ax + SLACK &&
          gy >= ay - SLACK && gy <= ay + SLACK);
    /*
     * X11 and zone placement answer with the position itself, so the test
     * above is a real comparison. A layer surface's margins are only believed:
     * bw_where hands back exactly what was asked for, the test compares a
     * number with itself and can never fail, so the belief is worth no more
     * than the screenshot that proves it - the same proof the move probe takes.
     */
    if (ok && !bw_where_live (w))
    {
        int proof = prove_places ? bw_verify_at (w) : -1;

        printf ("place: %s asked %d,%d got %d,%d %dx%d %s\n",
                what, ax, ay, gx, gy, gw, gh,
                (proof > 0) ? "(proved)"
              : (proof == 0) ? "(not there)"
                             : "(the protocol's word, unproven)");
        /* 0 alone convicts, and only when nothing else could be in the
           picture: a covered window is no answer either */
        if (proof == 0 && screen_is_ours ())
        {
            printf ("PLACE-IGNORED %s\n", what);
            ok = 0;
        }
        fflush (stdout);

        return ok;
    }
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
    bw_win *w;
    int n, x0, y0, x1, y1;
} seen[WATCHED];

/* What the Wayland proof concluded; X11 never doubts its own server */
static int wl_verified = 1;

void bench_watch (bw_win *w)
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
    bw_where (w, &x, &y, NULL, NULL);
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

/* What a window that has gone away left behind. Its box cannot be kept: the
   allocator hands the same address to the window opened next, and then the
   two of them widen one box - the reading this table is built to avoid */
static int retired_looked, retired_moved;

static int box_moved (int i)
{
    return (seen[i].x1 - seen[i].x0) >= REALLY_MOVED ||
           (seen[i].y1 - seen[i].y0) >= REALLY_MOVED;
}

void bench_unwatch (bw_win *w)
{
    int i;

    for (i = 0; i < WATCHED; i++)
    {
        if (seen[i].w == w && seen[i].n > 0)
        {
            if (seen[i].n >= 2)
            {
                retired_looked = 1;
                retired_moved |= box_moved (i);
            }
            seen[i].w = NULL;
            seen[i].n = 0;
        }
    }
}

int bench_moved (void)
{
    int i, looked = retired_looked;

    if (!wl_verified)
    {
        return 0;
    }
    if (retired_moved)
    {
        return 1;
    }
    for (i = 0; i < WATCHED; i++)
    {
        if (seen[i].n < 2)
        {
            continue;
        }
        looked = 1;
        if (box_moved (i))
        {
            return 1;
        }
    }

    return !looked;             /* nobody looked, so nobody may complain */
}

static int way = 1;             /* what the probe settled on */

void bench_move (bw_win *w, int x, int y, int width, int height)
{
    bw_move_raw (w, x, y, width, height, way);
}

void bench_wait_until (double due)
{
    while (bench_now () < due)
    {
        bw_pump ();
        usleep (200);
    }
}

int bench_aimable (bw_win *w)
{
    if (bw_win_aimable (w))
    {
        return 1;
    }
    printf ("this session gives a window no spot a capture could be "
            "aimed at\n");
    fflush (stdout);

    return 0;
}

void bench_flip_way (void)
{
    if (!bw_is_wayland ())
    {
        way = !way;
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
static int try_move (bw_win *w, int try_way, const char *name)
{
    int bx, by, gx, gy, i;

    bw_sync ();
    bw_where (w, &bx, &by, NULL, NULL);
    bw_move_raw (w, bx + STEP_X, by + STEP_Y, 0, 0, try_way);
    bw_sync ();
    for (i = 0; i < 20; i++)    /* up to two seconds */
    {
        usleep (100000);
        bw_pump ();
        bw_where (w, &gx, &gy, NULL, NULL);
        if (shifted (gx - bx, gy - by))
        {
            break;
        }
    }
    printf ("probe: %s moved %d,%d of %d,%d\n",
            name, gx - bx, gy - by, STEP_X, STEP_Y);
    fflush (stdout);

    return shifted (gx - bx, gy - by);
}

/*
 * The Wayland probe. The margins of a layer surface are the protocol's own
 * positioning, so a session that grants them at all grants every one - what
 * has to be proved is that this compositor really repositions the surface,
 * which one screenshot at the moved-to spot answers. Without a screenshot
 * command the moves are unproven and said to be, once, rather than refused:
 * the protocol promised, and nobody could look.
 */
static int wl_notice_printed;

/*
 * In the stress mix six programs share the screen and any of them, or the
 * pattern watching them, can legitimately sit over this window when the
 * screenshot is taken - and a photograph of somebody else's pixels convicts
 * nobody. Solo, the screen is this program's alone, so pixels that are not
 * ours mean the surface really is not where the protocol promised.
 */
static int screen_is_ours (void)
{
    const char *gate = getenv ("BENCH_GO");

    return gate == NULL || gate[0] == '\0';
}

static int wl_probe (bw_win *w, int x, int y, int width, int height)
{
    int bx, by, tries, proof = -1;

    if (bw_move_raw (w, x, y, 0, 0, 1) < 0)
    {
        printf ("moves: neither way works\n");
        printf ("MOVE-REFUSED this session does not move a mapped window\n");
        fflush (stdout);

        return -1;
    }
    if (bw_where_live (w))
    {
        /*
         * Zone placement reports positions back, so the X11 probe's own
         * logic applies unchanged: ask for a move, look at the session's
         * answer, and believe the answer over the request.
         */
        int got = -1;

        for (tries = 0; tries < 3 && got < 0; tries++)
        {
            if (try_move (w, 1, "zone placement"))
            {
                got = 1;
            }
        }
        bw_move_raw (w, x, y, width, height, 1);
        bw_sync ();
        usleep (300000);
        printf ("moves: %s\n", got > 0 ? "zone placement"
                                       : "neither way works");
        if (got < 0)
        {
            printf ("MOVE-REFUSED this session does not move a mapped "
                    "window\n");
            wl_verified = 0;
        }
        fflush (stdout);

        return got;
    }
    bw_where (w, &bx, &by, NULL, NULL);
    bw_move_raw (w, bx + STEP_X, by + STEP_Y, 0, 0, 1);
    bw_sync ();
    /* Three looks, like the X11 probe: one slow compositor frame is not
       a refusal */
    for (tries = 0; tries < 3; tries++)
    {
        usleep (300000);
        proof = bw_verify_at (w);
        if (proof != 0)
        {
            break;
        }
    }
    printf ("probe: layer-shell margins moved %d,%d of %d,%d%s\n",
            proof != 0 ? STEP_X : 0, proof != 0 ? STEP_Y : 0, STEP_X, STEP_Y,
            proof < 0 ? " (unproven)" : "");
    /* Back where it belongs */
    bw_move_raw (w, x, y, width, height, 1);
    bw_sync ();
    usleep (300000);
    if (proof == 0 && screen_is_ours ())
    {
        printf ("moves: neither way works\n");
        printf ("MOVE-REFUSED this session does not move a mapped window\n");
        fflush (stdout);
        wl_verified = 0;

        return -1;
    }
    if (proof <= 0 && !wl_notice_printed)
    {
        wl_notice_printed = 1;
        printf ("verify: %s, so the moves are taken on the protocol's word\n",
                proof < 0 ? "no way to photograph the screen"
                          : "another window sits over this one");
    }
    printf ("moves: layer-shell margins\n");
    fflush (stdout);

    return 1;
}

int bench_probe_move (bw_win *w, int x, int y, int width, int height)
{
    int got = -1, tries;

    if (bw_is_wayland ())
    {
        return wl_probe (w, x, y, width, height);
    }

    /*
     * Three times each way before giving up. Calling a session incapable of
     * moving a window is a heavy thing to say - the row it belongs to is
     * dropped - and one slow answer while the desktop is still settling is
     * not proof. Each attempt that fails has waited its full two seconds
     * first, so a session that moves nothing spends about twelve seconds
     * here before saying so.
     */
    for (tries = 0; tries < 3 && got < 0; tries++)
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
        if (try_move (w, 1, "plain client calls"))
        {
            got = 1;
        }
        else if (try_move (w, 0, "the pager request"))
        {
            got = 0;
        }
    }

    /* Back where it belongs, by whichever way works */
    bw_move_raw (w, x, y, width, height, (got == 1));
    bw_sync ();
    usleep (300000);

    printf ("moves: %s\n", (got == 0) ? "the pager request"
                         : (got == 1) ? "plain client calls"
                                      : "neither way works");
    if (got < 0)
    {
        printf ("MOVE-REFUSED this session does not move a mapped window\n");
    }
    fflush (stdout);
    /*
     * A session that honours neither way is left on the pager request, which
     * is what the old callers did with a refusal: the row is usually dropped
     * anyway, but the phases that carry on - resizing, drag and drop - must
     * not quietly change which path through the window manager they take.
     */
    way = (got >= 0) ? got : 0;

    return got;
}

void bench_verify_end (bw_win *w)
{
    if (!bw_is_wayland ())
    {
        return;
    }
    /* 0 alone convicts, and only when nothing else could be in the picture:
       no screenshot is no answer, and a covered window is no answer either */
    if (bw_verify_at (w) == 0 && screen_is_ours ())
    {
        wl_verified = 0;
    }
}
