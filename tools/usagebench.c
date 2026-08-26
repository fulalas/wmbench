/*
 * A person using the desktop, scripted: two windows go through what real
 * windows go through. Maximize and back. Minimize and back. Walked to the
 * left and right screen edges and snapped to half the screen. Walked to the
 * four corners, clockwise and counter-clockwise, and snapped to a quarter.
 * A text document scrolled smoothly, first with the scroll bar's chevrons
 * and then by dragging its thumb. Resized by dragging each handle, corner and
 * edge, on screen throughout. Icons dragged one by one from one window into
 * another and then all carried back at once. Raised over each other the way
 * alt-tab does. Fullscreen and back.
 *
 * Nothing injects input, so the run is the same on every window manager and
 * display server - which is the point: the numbers are only comparable when
 * every session did identical work.
 *
 *   usagebench <seconds> [actions per second] [phase]
 *
 * BENCH_TASKS=N runs exactly N passes instead of stopping on the clock.
 *
 * The phase is all (the default), windows, scroll, resize or dnd, so one part
 * can be measured on its own.
 *
 * BENCH_CHECKPOINT_DIR=<dir>: run exactly one pass instead of a timed loop,
 * and after every deterministic step write a checkpoint there: a line in
 * manifest.txt with the geometry that was asked for and the geometry the WM
 * actually gave, plus a screenshot of the window's own content (through
 * bw_capture, so it works on Wayland too). Two sessions' checkpoint folders
 * can then be compared pixel by pixel with compare_runs.sh: the content is
 * ours alone, so the desktop's own looks never enter the comparison.
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

#define WINW 900                /* the size a big screen uses */
#define WINH 700
#define BAND 40
#define NCOL 6

/* The icon views that drag and drop needs */
#define ICON    72
#define CELL   120
#define COLS      4
#define NICON     8
/* The gap the two views keep between them, and the least width one can have */
#define DNDGAP   40
#define DNDMINW  (40 + (COLS - 1) * CELL + ICON + 40)

/*
 * Room left around everything: window managers put a frame around the window
 * and a panel at the edges, and a frame that reaches past the screen is
 * handled differently by each of them - KDE pushed it off the display where
 * others let it grow. Every target stays this far inside.
 */
#define SAFE    120

/* The text document and its scroll bar */
#define LINEH   22              /* one line of text */
#define DOCLINES 200            /* how long the document is */
#define SBW     24              /* the scroll bar */
#define CHEV    24              /* its chevron buttons */

/*
 * The window size actually used, and the patch of screen everything is laid
 * out in: 1920x1080 or the screen if smaller. Maximising, snapping to half
 * the screen and going fullscreen are the window manager's own geometry and
 * use the real screen, sw and sh below.
 */
static int winw = WINW, winh = WINH;
static int stage_x, stage_y, stage_w, stage_h;
static bw_win *wa, *wb, *carrier[4];
static int sw, sh, actions = 0;
static bw_win *doc_buf;         /* the document is drawn here, copied once */
static bw_win *bands_buf;       /* the bands, so a resize copies instead of drawing */
static bw_win *bands_buf_b;     /* the same for the second window, its own offset */
static const char *phase;       /* which part of the pass to run */
static int fixed;               /* a fixed number of passes, not a clock */
static int doc_px;              /* how far it is scrolled, in pixels */
static double pace;
static double run_start, run_seconds;   /* the clock, when there is one */
static const char *ckdir;
static FILE *manifest;
static int cknum;
static int state_wrong;         /* a state the compositor never granted */

static const unsigned long palette[NCOL] = {
    0xc04040, 0x40c040, 0x4040c0, 0xc0c040, 0xc040c0, 0x40c0c0
};

/*
 * Fixed work: BENCH_TASKS=N does exactly N passes of the chosen phase, however
 * long that takes, so every session performs the same amount of work. The
 * measured part is bracketed by the two marks.
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

static int time_up (void);

static void step (void)
{
    bw_sync ();
    /*
     * On the clock, once the time is up the rest of the pass is walked
     * through without the pause between actions: the phases stop at their
     * own boundaries, and waiting a third of a second at each remaining one
     * is how five seconds used to turn into ten. Those hurried actions are
     * not counted either, or the rate at the end would be a rate nobody
     * worked at.
     */
    if (time_up ())
    {
        return;
    }
    actions++;
    usleep ((useconds_t) (pace * 1e6));
    bw_pump ();
}

static void draw_content_size (bw_win *w, int offset, int width, int height)
{
    int start = -(((offset % BAND) + BAND) % BAND);
    int j = 0, y;

    for (y = start; y < height; y += BAND, j++)
    {
        bw_fill (w, palette[(((offset / BAND + j) % NCOL) + NCOL) % NCOL],
                 0, y, width, BAND);
    }
}

/* The whole screen's worth, so the window has content at any size */
static void draw_content (bw_win *w, int offset)
{
    draw_content_size (w, offset, sw, sh);
    bw_present (w);
}

/*
 * One checkpoint: what was asked, what the WM did, and what the window's own
 * content looks like, saved as a PPM cut to the asked size at the actual
 * position - identical content across sessions unless something differs.
 */
static void checkpoint_of (bw_win *w, const char *name,
                           int ax, int ay, int aw, int ah)
{
    bw_image *img;
    int rx, ry, gw, gh;

    if (ckdir == NULL)
    {
        return;
    }
    bw_sync ();
    usleep (250000);            /* let the WM finish whatever it does */

    bw_where (w, &rx, &ry, &gw, &gh);
    fprintf (manifest, "%02d %-14s asked %d,%d %dx%d  got %d,%d %dx%d\n",
             cknum, name, ax, ay, aw, ah, rx, ry, gw, gh);
    fflush (manifest);

    /* Clamp to the screen; what falls outside cannot be photographed */
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (aw > sw - rx) aw = sw - rx;
    if (ah > sh - ry) ah = sh - ry;
    /*
     * A window the WM left past the right or bottom edge clamps to nothing,
     * and a position that is not a screen coordinate - an unplaced Wayland
     * toplevel, or zone placement, whose origin is opaque by design - is
     * nothing to aim a photograph at. The count still moves on, so the
     * numbering keeps step with the manifest line written above.
     */
    if (aw <= 0 || ah <= 0 || !bw_win_aimable (w))
    {
        cknum++;

        return;
    }
    img = bw_capture (rx, ry, aw, ah);
    if (img != NULL)
    {
        char path[512];

        snprintf (path, sizeof path, "%s/ck-%02d-%s.ppm", ckdir, cknum, name);
        capture_write_ppm (path, img);
        bw_image_free (img);
    }
    cknum++;
}

static void checkpoint (const char *name, int ax, int ay, int aw, int ah)
{
    checkpoint_of (wa, name, ax, ay, aw, ah);
}

static int want_phase (const char *name)
{
    return strcmp (phase, "all") == 0 || strcmp (phase, name) == 0;
}

/*
 * Fixed work runs to the last task whatever the clock says. On the clock,
 * a pass is longer than any sensible number of seconds, so the phases ask
 * between steps whether the time is up instead of overrunning by a whole one.
 */
static int time_up (void)
{
    return !fixed && bench_now () - run_start >= run_seconds;
}

static int scene_wrong;         /* the windows are not where the phase needs them */

/*
 * On Wayland a state is granted or it is not, and the compositor says which
 * in its own configure events; asked for and never granted has to be caught,
 * or the phase measures a window that sat still. On X11 the moves themselves
 * are watched instead, the way they always were.
 */
static void expect_state (bw_win *w, unsigned mask, int on, const char *what)
{
    int i;

    if (!bw_is_wayland () || time_up ())
    {
        return;
    }
    for (i = 0; i < 20; i++)    /* up to two seconds */
    {
        unsigned st = bw_state (w);

        if (on ? (st & mask) != 0 : (st & mask) == 0)
        {
            return;
        }
        usleep (100000);
    }
    if (!state_wrong)
    {
        printf ("STATE-REFUSED the compositor never granted: %s\n", what);
        fflush (stdout);
    }
    state_wrong = 1;
}

/* Back to a known place and size, whatever the last phase left behind */
static void reset_window (bw_win *w, int x, int y)
{
    if (time_up ())
    {
        return;
    }
    bw_maximize (w, 0);
    bw_fullscreen (w, 0);
    bench_move (w, x, y, winw, winh);
    step ();
}

/*
 * Walk the window in steps, the way a drag carries it, until its centre sits
 * on the given point - the middle of an edge, or a corner - the way a person
 * drags to tile. Then snap it to the given rectangle, what edge or corner
 * tiling leaves behind. Plain moves, so it is identical under every WM. On
 * Wayland nothing moves a managed toplevel, so the walk collapses and the
 * snap is the client resizing itself to the tile's size, which is the half
 * of the job a client is allowed.
 */
static void walk_and_snap (bw_win *w, int cx, int cy,
                           int tx, int ty, int tw, int th)
{
    int x0, y0, gw, gh, wx, wy, i;

    if (time_up ())
    {
        return;
    }
    if (bw_win_placed (w))
    {
        bw_where (w, &x0, &y0, &gw, &gh);
        wx = cx - gw / 2;
        wy = cy - gh / 2;
        for (i = 1; i <= 12; i++)
        {
            bench_move (w, x0 + (wx - x0) * i / 12, y0 + (wy - y0) * i / 12, 0, 0);
            bw_sync ();
            usleep (20000);
        }
    }
    else
    {
        /*
         * Nothing a client says moves a managed toplevel, so the walk is
         * carried by the size instead. Dropping it outright left the Wayland
         * pass a hundred-odd composited steps lighter than the X11 one it is
         * compared with.
         */
        bw_where (w, NULL, NULL, &gw, &gh);
        for (i = 1; i <= 12; i++)
        {
            bw_resize (w, gw + (tw - gw) * i / 12, gh + (th - gh) * i / 12);
            bw_sync ();
            usleep (20000);
        }
    }
    bench_move (w, tx, ty, tw, th);
    step ();
    /*
     * Where it really went. A compositor can take every one of these and act
     * on none of them, and then the phase measured a window sitting still.
     */
    bench_watch (w);
}

/*
 * A different sentence on every line, mixed by a fixed rule rather than
 * rand(), so every session renders exactly the same document.
 */
static void line_text (int n, char *out, int len)
{
    static const char *w1[] = { "The", "A", "One", "Some", "That", "Every",
                                "Another", "This" };
    static const char *w2[] = { "quick", "lazy", "bright", "silent", "heavy",
                                "gentle", "curious", "stubborn", "narrow",
                                "ancient", "polished", "crooked" };
    static const char *w3[] = { "fox", "engine", "river", "window", "compass",
                                "letter", "garden", "mirror", "ladder",
                                "bridge", "kettle", "signal", "anchor" };
    static const char *w4[] = { "jumps over", "runs past", "waits behind",
                                "slides under", "leans against",
                                "drifts toward", "circles around",
                                "settles beside", "hums near" };
    static const char *w5[] = { "the sleeping dog", "an open door",
                                "the old pier", "a broken clock",
                                "the wet pavement", "a stack of books",
                                "the far hill", "a quiet room",
                                "the last train", "an empty street",
                                "the low wall" };
    static const char *w6[] = { "before dawn.", "after the rain.",
                                "in plain sight.", "without a sound.",
                                "once again.", "for no reason.", "as always.",
                                "by accident.", "against the wind.",
                                "under the lamp." };
    unsigned int m = (unsigned int) n * 2654435761u;

    snprintf (out, (size_t) len, "%4d  %s %s %s %s %s %s", n + 1,
              w1[m % 8], w2[(m >> 3) % 12], w3[(m >> 7) % 13],
              w4[(m >> 11) % 9], w5[(m >> 15) % 11], w6[(m >> 19) % 10]);
}

static int doc_max_px (void)
{
    return DOCLINES * LINEH - winh;
}

/*
 * The document at its current pixel offset, with the scroll bar beside it:
 * track, thumb sized to what is visible, and a chevron at each end, darkened
 * while it is being clicked. Drawn into a buffer and copied in one go, so any
 * flicker on screen is the compositor's and not ours.
 */
static void draw_document (int press_up, int press_down)
{
    char line[160];
    int first = doc_px / LINEH, shift = doc_px % LINEH, i, y, track, th, ty;
    bw_point tri[3];

    bw_fill (doc_buf, 0xffffff, 0, 0, winw, winh);

    /* One line beyond each edge, clipped, so the edges cut cleanly */
    bw_clip (doc_buf, 0, 0, winw - SBW, winh);
    for (i = -1; i * LINEH - shift < winh; i++)
    {
        if (first + i < 0 || first + i >= DOCLINES)
        {
            continue;
        }
        y = 18 + i * LINEH - shift;
        line_text (first + i, line, sizeof line);
        bw_text (doc_buf, 0x202020, 12, y, line);
    }
    bw_clip (doc_buf, 0, 0, -1, -1);

    bw_fill (doc_buf, 0xd8d8d8, winw - SBW, 0, SBW, winh);
    bw_fill (doc_buf, press_up ? 0x606060 : 0xb8b8b8, winw - SBW, 0, SBW, CHEV);
    bw_fill (doc_buf, press_down ? 0x606060 : 0xb8b8b8,
             winw - SBW, winh - CHEV, SBW, CHEV);
    tri[0].x = winw - SBW + 4;  tri[0].y = CHEV - 7;
    tri[1].x = winw - 4;        tri[1].y = CHEV - 7;
    tri[2].x = winw - SBW / 2;  tri[2].y = 6;
    bw_poly (doc_buf, 0x202020, tri, 3);
    tri[0].x = winw - SBW + 4;  tri[0].y = winh - CHEV + 7;
    tri[1].x = winw - 4;        tri[1].y = winh - CHEV + 7;
    tri[2].x = winw - SBW / 2;  tri[2].y = winh - 6;
    bw_poly (doc_buf, 0x202020, tri, 3);

    track = winh - 2 * CHEV;
    th = track * winh / (DOCLINES * LINEH);
    ty = CHEV + (track - th) * doc_px / (doc_max_px () ? doc_max_px () : 1);
    bw_fill (doc_buf, 0x707070, winw - SBW + 3, ty, SBW - 6, th);

    bw_copy (doc_buf, wa, 0, 0, winw, winh, 0, 0);
    bw_present (wa);
    bw_sync ();
}

/*
 * Smooth scrolling: the offset is carried to its target a few pixels at a
 * time, one frame every 16 ms, which is what the compositor has to keep up
 * with. Nothing here is timed by the machine, so every session scrolls the
 * same pixels the same number of times.
 */
static void doc_glide (int to, int frames, int press_up, int press_down)
{
    int from = doc_px, i;

    if (time_up ())
    {
        return;
    }
    if (to < 0)
    {
        to = 0;
    }
    if (to > doc_max_px ())
    {
        to = doc_max_px ();
    }
    for (i = 1; i <= frames; i++)
    {
        doc_px = from + (to - from) * i / frames;
        draw_document (press_up, press_down);
        usleep (16000);
    }
    doc_px = to;
    draw_document (0, 0);
}

/*
 * The band pattern, drawn once at screen size. A resize then copies from it
 * instead of drawing band by band into the window: one operation a frame, so
 * what is on screen is never half-painted. Drawing straight into the window
 * flickered visibly under KWin and left artifacts under a WM that does not
 * composite at all.
 */
static void bands_ready (void)
{
    if (bands_buf != NULL)
    {
        return;
    }
    bands_buf = bw_canvas (stage_w, stage_h);
    draw_content_size (bands_buf, 0, stage_w, stage_h);

    /*
     * The pattern is also the window's background. Growing a window exposes
     * new strips, and the server fills them from the background before the
     * application can paint: with a plain colour that is a flash of black
     * every step (flicker under KWin, artifacts where nothing composites),
     * and with no background at all the strip is undefined, which reads as
     * the window losing its content. Filled from the pattern it is right
     * either way, so nothing wrong is ever on screen.
     */
    bw_set_background (wa, bands_buf);

    /*
     * The second window needs one too. Without a background it keeps the black
     * it was created with, and nothing here listens for Expose, so every strip
     * of it that the resizing window uncovers turns black and stays black
     * until something else happens to paint it. That reads as a black edge
     * flashing along the window being resized, on every step.
     */
    if (bands_buf_b == NULL)
    {
        bands_buf_b = bw_canvas (stage_w, stage_h);
        draw_content_size (bands_buf_b, 3 * BAND, stage_w, stage_h);
    }
    bw_set_background (wb, bands_buf_b);
    bw_sync ();
}

/*
 * Aim at fixed marks instead of sleeping a fixed time. Sleeping 16 ms after
 * the work is done makes every step 16 ms plus the work, which slides against
 * the screen's refresh and doubles or drops a frame every so often - the
 * stutter. A step that lands late takes its next mark from now rather than
 * letting the lateness pile up.
 */
static void pace_step (struct timespec *next, long period_us)
{
    struct timespec now;

    next->tv_nsec += period_us * 1000L;
    while (next->tv_nsec >= 1000000000L)
    {
        next->tv_nsec -= 1000000000L;
        next->tv_sec += 1;
    }
    clock_gettime (CLOCK_MONOTONIC, &now);
    if (now.tv_sec > next->tv_sec ||
        (now.tv_sec == next->tv_sec && now.tv_nsec > next->tv_nsec))
    {
        *next = now;

        return;
    }
    clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

/*
 * Resize by dragging one handle: the rectangle glides to its target a few
 * pixels a frame and the content is redrawn to every new size, which is what
 * an application does while a person drags its edge. Every step is a fresh
 * window pixmap for the compositor, the most expensive thing it has to do.
 */
static void drag_resize (int x0, int y0, int w0, int h0,
                         int x1, int y1, int w1, int h1, int frames)
{
    int i, x, y, w, h;
    struct timespec next;

    if (time_up ())
    {
        return;
    }
    clock_gettime (CLOCK_MONOTONIC, &next);
    for (i = 1; i <= frames; i++)
    {
        x = x0 + (x1 - x0) * i / frames;
        y = y0 + (y1 - y0) * i / frames;
        w = w0 + (w1 - w0) * i / frames;
        h = h0 + (h1 - h0) * i / frames;
        bench_move (wa, x, y, w, h);
        /*
         * Nothing is drawn here on purpose. The pattern is the window's
         * background, so the server fills every strip the window gains with
         * exactly what belongs there, at the moment it gains it. Anything this
         * program draws per step is drawn for a size the window may not have
         * yet, and a whole window redrawn while it is also moving tears.
         */
        bw_sync ();
        pace_step (&next, 16000);
    }

    /*
     * Land exactly on the target. A stream of sixty requests can leave the
     * window manager a step behind, and then the checkpoint would record a
     * size nobody asked for.
     */
    bench_move (wa, x1, y1, w1, h1);
    bw_sync ();
    usleep (250000);

    /*
     * Did it take? A compositor can honour the pager request for the position
     * and ignore the size (labwc does), and then a resize test resizes
     * nothing. What it must not be is a test for "the size we asked for": a
     * window manager is free to grant a smaller one, and one does on the leg
     * that drags the top left corner, and reading that as a refusal flipped
     * the method on every leg - half of them then went through a different
     * path in the window manager, which is not what the numbers are for.
     * A leg whose size never budged at all is the real refusal.
     */
    {
        int gw, gh;

        bw_where (wa, NULL, NULL, &gw, &gh);
        if ((gw == w0) && (gh == h0) && ((w1 != w0) || (h1 != h0)))
        {
            bench_flip_way ();
            bench_move (wa, x1, y1, w1, h1);
            bw_sync ();
            usleep (250000);
        }
    }
    bw_copy (bands_buf, wa, 0, 0, w1, h1, 0, 0);
    bw_present (wa);
    bw_sync ();
}

/* The four handles a person grabs, each out and back, never off the screen */
static void resize_phase (int bx, int by)
{
    int gw = winw + 600, gh = winh + 400, tx, ty;


    /* Grow only as far as the stage allows, frame and panels included */
    if (bx + gw > stage_x + stage_w) gw = stage_x + stage_w - bx;
    if (by + gh > stage_y + stage_h) gh = stage_y + stage_h - by;

    /* The bottom-right corner */
    drag_resize (bx, by, winw, winh, bx, by, gw, gh, 60);
    checkpoint ("resize-corner", bx, by, gw, gh);
    drag_resize (bx, by, gw, gh, bx, by, winw, winh, 60);
    step ();

    /* The right edge alone */
    drag_resize (bx, by, winw, winh, bx, by, gw, winh, 60);
    checkpoint ("resize-right", bx, by, gw, winh);
    drag_resize (bx, by, gw, winh, bx, by, winw, winh, 60);
    step ();

    /* The bottom edge alone */
    drag_resize (bx, by, winw, winh, bx, by, winw, gh, 60);
    checkpoint ("resize-bottom", bx, by, winw, gh);
    drag_resize (bx, by, winw, gh, bx, by, winw, winh, 60);
    step ();

    /*
     * The top-left corner, which moves the window as it grows. The
     * bottom-right stays where it is and the target is clamped to the screen,
     * so no window manager has to decide what to do with an edge that left.
     */
    tx = bx - 500;
    ty = by - 140;
    if (tx < SAFE) tx = SAFE;
    if (ty < SAFE) ty = SAFE;
    drag_resize (bx, by, winw, winh,
                 tx, ty, winw + (bx - tx), winh + (by - ty), 60);
    checkpoint ("resize-topleft", tx, ty, winw + (bx - tx), winh + (by - ty));
    drag_resize (tx, ty, winw + (bx - tx), winh + (by - ty),
                 bx, by, winw, winh, 60);
    step ();
}

/*
 * The same four carrier windows are used for every drag, mapped when one
 * starts and unmapped when it ends - which is worth testing in itself, and
 * is where window managers part company. The catch is that a map or an unmap
 * is a request, not a fact: asking and then carrying on assumes the pair is
 * acted on in order, and under XWayland it is not, which left a carrier
 * invisible and made the icon look like it teleported. So each one is waited
 * for. BW_NOTIFY on the carriers is what makes the wait possible.
 */
static void show_carrier (bw_win *c, int x, int y)
{
    bw_move_raw (c, x, y, 0, 0, 1);
    bw_map (c);
    bw_raise (c);
    if (!bw_wait_shown (c, 1))
    {
        printf ("carrier did not map\n");
        fflush (stdout);
    }
    /*
     * Place and raise it again now that it really is mapped. A window being
     * shown for the second time comes back where it was in the stack, which
     * under XWayland means behind the windows it is supposed to fly over -
     * the icon then looks like it teleported. A freshly created window is on
     * top by luck, not by right, so the raise is what actually matters.
     */
    bw_move_raw (c, x, y, 0, 0, 1);
    bw_raise (c);
    bw_sync ();
    usleep (40000);             /* let the surface exist before it is drawn */
}

static void hide_carrier (bw_win *c)
{
    const char *ms = getenv ("BENCH_DND_SETTLE");

    bw_unmap (c);
    if (!bw_wait_shown (c, 0))
    {
        printf ("carrier did not unmap\n");
        fflush (stdout);
    }
    /*
     * Let the compositor finish putting the window away before it is asked
     * for again. The X server has it unmapped by now, but a Wayland
     * compositor also tears down the surface behind it, and mapping the same
     * window again while that is in flight left labwc showing nothing at all
     * - the icon appeared to teleport. BENCH_DND_SETTLE=0 turns the wait off
     * to see the difference.
     */
    usleep ((useconds_t) ((ms != NULL ? atoi (ms) : 250) * 1000));
}

static void icon_cell (int i, int *cx, int *cy)
{
    *cx = 40 + (i % COLS) * CELL;
    *cy = 60 + (i / COLS) * (CELL + 30);
}

/* One icon: a sheet with a folded corner and a label bar under it */
static void draw_icon (bw_win *t, int x, int y, int i)
{
    bw_point fold[3];

    bw_fill (t, palette[i % NCOL], x, y, ICON, ICON);
    fold[0].x = x + ICON - 22; fold[0].y = y;
    fold[1].x = x + ICON;      fold[1].y = y;
    fold[2].x = x + ICON;      fold[2].y = y + 22;
    bw_poly (t, 0xffffff, fold, 3);
    bw_rect (t, 0x303030, x, y, ICON, ICON);
    bw_fill (t, 0x303030, x + 8, y + ICON + 8, ICON - 16, 6);
    bw_present (t);
}

/* The two icon views: what is still here, what has been dropped, and whether
 * a drag is hovering over this one */
static void draw_view (bw_win *w, const char *title, const int *have,
                       int count, int hot)
{
    int i, cx, cy;

    bw_fill (w, 0xf0f0f0, 0, 0, winw, winh);
    bw_rect (w, hot ? 0x3070d0 : 0xc0c0c0, 24, 48, winw - 48, winh - 72);
    bw_rect (w, hot ? 0x3070d0 : 0xc0c0c0, 25, 49, winw - 50, winh - 74);
    if (hot)
    {
        bw_rect (w, 0x3070d0, 26, 50, winw - 52, winh - 76);
    }
    bw_text (w, 0x404040, 40, 36, title);

    for (i = 0; i < count; i++)
    {
        icon_cell (i, &cx, &cy);
        if (have[i] >= 0)
        {
            draw_icon (w, cx, cy, have[i]);
        }
        else
        {
            /* The gap an icon left behind */
            bw_rect (w, 0xb0b0b0, cx, cy, ICON, ICON);
        }
    }
    bw_present (w);
    bw_sync ();
}

/*
 * Icons dragged from one window into another. What the compositor sees is a
 * small frameless translucent window travelling over another window, which is
 * exactly what a drag is; the last leg carries four of them at once.
 *
 * The views have to sit at known spots for the flights to make sense. On
 * X11 that is wa and wb moved side by side; on Wayland a managed toplevel
 * sits wherever the compositor put it, so the phase plays out on two
 * layer-shell windows of its own - and where there is no layer-shell the
 * scene cannot be built and the phase says not done.
 */
static void dnd_phase (int bx, int by)
{
    bw_win *va, *vb;
    int home[NICON], album[NICON];
    int order[4] = { 0, 3, 5, 6 };
    int k, f, i, cx, cy, tx, ty, x, y, nalbum = 0;
    int wide = winw;
    int lx, rx;
    char ckname[32];

    /*
     * The two views sit side by side with a gap between them, and both get
     * narrower when the stage cannot hold two of the usual width. They used
     * to keep that width whatever the screen was, which left them touching or
     * overlapping, and a drag that crosses from one window into the other has
     * to have somewhere to cross.
     */
    if (2 * winw + DNDGAP > stage_w)
    {
        winw = (stage_w - DNDGAP) / 2;
        if (winw < DNDMINW)
        {
            /* Narrower than the icons themselves is no use; they overlap */
            winw = DNDMINW;
        }
    }
    lx = stage_x;
    rx = stage_x + stage_w - winw;
    for (i = 0; i < NICON; i++)
    {
        home[i] = i;
        album[i] = -1;
    }

    /*
     * The managed windows step aside and the drag plays out on two windows
     * this program places itself, on either session: a managed Wayland
     * toplevel cannot be put side by side with another at all, and doing it
     * one way here and another way there would drag icons across two
     * different scenes.
     */
    bw_unmap (wa);
    bw_unmap (wb);
    va = bw_create (NULL, lx, by, winw, winh, "usagebench A",
                    BW_PLACED | BW_KEEP | BW_UNMANAGED);
    vb = bw_create (NULL, rx, by, winw, winh, "usagebench B",
                    BW_PLACED | BW_KEEP | BW_UNMANAGED);
    if (va == NULL || vb == NULL)
    {
        scene_wrong = 1;

        return;
    }
    bw_map (va);
    bw_map (vb);
    bw_sync ();
    usleep (300000);
    /*
     * The two views have to be side by side. Where they are is the question
     * here, not whether they moved: dragging icons into a window hidden
     * behind the one they left is not what this measures, however the two of
     * them came to be stacked that way.
     */
    /* This phase runs after MEASURE-START, and the screenshot that proves a
       believed position costs about 0.3 s that would land in the numbers */
    bench_prove_places (0);
    if (!bench_placed (va, lx, by, "usagebench A"))
    {
        scene_wrong = 1;
    }
    if (!bench_placed (vb, rx, by, "usagebench B"))
    {
        scene_wrong = 1;
    }
    bench_prove_places (1);
    bw_sync ();
    usleep (300000);
    draw_view (va, "Pictures", home, NICON, 0);
    draw_view (vb, "Album", album, 0, 0);
    step ();

    /*
     * One carrier for every drag of this phase, shown once and then simply
     * moved: while a drag is not happening it is parked exactly over the icon
     * it will pick up next, which looks like that icon sitting in its slot.
     *
     * It used to be unmapped after each drop and mapped again for the next
     * drag. That is a fair thing to ask of a compositor - and popbench asks
     * it twenty times a second - but labwc brings a re-mapped unmanaged
     * window back in the wrong layer, so the icon was simply not on screen
     * for whole flights and looked like it teleported. Raising it every frame
     * put it back for a frame at a time, which flickered. Parking instead of
     * cycling keeps the one window and shows every drag.
     */
    icon_cell (order[0], &cx, &cy);
    /* A session that places the views but cannot show a carrier - zones
       without layer-shell - has no drag to composite, and that is the
       phase, not a detail of it */
    if (!bw_win_placed (carrier[0]))
    {
        scene_wrong = 1;
    }
    show_carrier (carrier[0], lx + cx, by + cy);
    draw_icon (carrier[0], 1, 1, order[0]);
    bw_sync ();

    for (k = 0; k < 4 && !time_up (); k++)
    {
        i = order[k];
        icon_cell (i, &cx, &cy);
        icon_cell (nalbum, &tx, &ty);

        /* Lift: the slot empties and what was over it is now being carried */
        home[i] = -1;
        draw_view (va, "Pictures", home, NICON, 0);
        bw_sync ();
        usleep (150000);
        snprintf (ckname, sizeof ckname, "dnd-lift-%d", k + 1);
        checkpoint_of (va, ckname, lx, by, winw, winh);

        /* Carried across in an arc, the way a hand moves */
        for (f = 1; f <= 55 && !time_up (); f++)
        {
            double t = (double) f / 55.0;

            x = (int) ((lx + cx) + ((rx + tx) - (lx + cx)) * t);
            y = (int) ((by + cy) + ((by + ty) - (by + cy)) * t
                       - 90.0 * t * (1.0 - t) * 4.0);
            bw_move_raw (carrier[0], x, y, 0, 0, 1);
            draw_icon (carrier[0], 1, 1, i);
            if (f == 28)
            {
                draw_view (vb, "Album", album, nalbum, 1);
            }
            bw_sync ();
            usleep (16000);
        }

        /* Dropped: the album has it now */
        album[nalbum++] = i;
        draw_view (vb, "Album", album, nalbum, 0);

        /*
         * And the carrier moves straight on to whatever it picks up next -
         * the next icon in the source, or the album's first icon when the
         * single drags are done and all four are about to come back.
         */
        if (k + 1 < 4)
        {
            icon_cell (order[k + 1], &cx, &cy);
            bw_move_raw (carrier[0], lx + cx, by + cy, 0, 0, 1);
            draw_icon (carrier[0], 1, 1, order[k + 1]);
        }
        else
        {
            icon_cell (0, &cx, &cy);
            bw_move_raw (carrier[0], rx + cx, by + cy, 0, 0, 1);
            draw_icon (carrier[0], 1, 1, order[0]);
        }
        bw_sync ();
        step ();
        snprintf (ckname, sizeof ckname, "dnd-drop-%d", k + 1);
        checkpoint_of (vb, ckname, rx, by, winw, winh);
    }

    /* All four home again at once: four carriers travelling together */
    {
        int ax, ay, hx, hy;

        icon_cell (0, &cx, &cy);
        ax = rx + cx;
        ay = by + cy;
        icon_cell (0, &tx, &ty);
        hx = lx + tx;
        hy = by + ty;

        /* Three more, each parked over the album icon it will carry */
        for (k = 1; k < 4; k++)
        {
            icon_cell (k, &cx, &cy);
            show_carrier (carrier[k], rx + cx, by + cy);
            draw_icon (carrier[k], 1, 1, order[k]);
        }
        bw_sync ();
        usleep (200000);

        /* The album lets go of all four, and they are all being carried */
        nalbum = 0;
        for (i = 0; i < NICON; i++)
        {
            album[i] = -1;
        }
        draw_view (vb, "Album", album, 0, 0);
        for (k = 0; k < 4; k++)
        {
            icon_cell (k, &cx, &cy);
            bw_move_raw (carrier[k], ax + (rx + cx - ax), ay + (by + cy - ay),
                         0, 0, 1);
            draw_icon (carrier[k], 1, 1, order[k]);
        }
        bw_sync ();
        usleep (300000);

        for (f = 1; f <= 55 && !time_up (); f++)
        {
            double t = (double) f / 55.0;

            x = (int) (ax + (hx - ax) * t);
            y = (int) (ay + (hy - ay) * t - 90.0 * t * (1.0 - t) * 4.0);
            for (k = 0; k < 4; k++)
            {
                bw_move_raw (carrier[k], x + k * 14, y + k * 10, 0, 0, 1);
                draw_icon (carrier[k], 1, 1, order[k]);
            }
            if (f == 28)
            {
                draw_view (va, "Pictures", home, NICON, 1);
            }
            bw_sync ();
            usleep (16000);
        }

        /* Home: the icons are back in their slots and the carriers are done */
        for (k = 0; k < 4; k++)
        {
            home[order[k]] = order[k];
        }
        draw_view (va, "Pictures", home, NICON, 0);
        bw_sync ();
        for (k = 0; k < 4; k++)
        {
            hide_carrier (carrier[k]);
        }
        step ();
        checkpoint_of (va, "dnd-all-back", lx, by, winw, winh);
    }

    /* Put the two windows back at the width and place the other phases expect */
    winw = wide;
    bw_destroy (va);
    bw_destroy (vb);
    bw_map (wa);
    bw_map (wb);
    bench_move (wb, stage_x + (stage_w - winw) / 2 + 80, stage_y + 40,
              winw, winh);
    bench_move (wa, bx, by, winw, winh);
    draw_content (wa, 0);
    draw_content (wb, 3 * BAND);
    bw_sync ();
    step ();
}

static bw_win *make_window (int x, int y, const char *name)
{
    unsigned flags = BW_NOTIFY | BW_KEEP | BW_PLACED;
    int stated = strcmp (phase, "windows") == 0 || strcmp (phase, "all") == 0;
    bw_win *w;

    /*
     * The windows phase walks states that belong to a managed toplevel and to
     * nothing else, so there the window stays managed, and on Wayland it must
     * never land on a layer surface however placeable those are.
     *
     * Every other phase needs its own coordinates, and asking a manager for
     * them is not enough: labwc put both windows in one place and the second
     * was never seen at all, so those runs composited one window where the
     * X11 runs composited two. They go unmanaged on both sides instead, which
     * is the one scene rather than two that look alike.
     */
    if (stated)
    {
        if (bw_is_wayland ())
        {
            flags |= BW_STATED;
        }
    }
    else
    {
        flags |= BW_UNMANAGED;
    }
    w = bw_create (NULL, x, y, winw, winh, name, flags);
    bw_map (w);

    return w;
}

int main (int argc, char **argv)
{
    double seconds = (argc > 1) ? atof (argv[1]) : 30.0;
    double rate = (argc > 2) ? atof (argv[2]) : 3.0;
    int i, base_x, base_y;
    long tasks, pass = 0;
    double start;

    phase = (argc > 3) ? argv[3] : "all";
    if (strcmp (phase, "all") != 0 && strcmp (phase, "windows") != 0 &&
        strcmp (phase, "scroll") != 0 && strcmp (phase, "resize") != 0 &&
        strcmp (phase, "dnd") != 0)
    {
        fprintf (stderr, "phase must be all, windows, scroll, resize or dnd\n");

        return 2;
    }

    if (rate <= 0.0)
    {
        rate = 3.0;
    }
    pace = 1.0 / rate;

    ckdir = getenv ("BENCH_CHECKPOINT_DIR");
    if (ckdir != NULL && ckdir[0] != '\0')
    {
        char path[512];

        snprintf (path, sizeof path, "%s/manifest.txt", ckdir);
        manifest = fopen (path, "w");
        if (manifest == NULL)
        {
            fprintf (stderr, "cannot write in %s\n", ckdir);

            return 2;
        }
    }
    else
    {
        ckdir = NULL;
    }

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    bw_screen_size (&sw, &sh);
    /*
     * Everything this program places goes inside the stage, so it fits a
     * 1080p screen and does the same amount of work on any screen.
     */
    bw_stage (SAFE, &stage_x, &stage_y, &stage_w, &stage_h);
    if (winw > stage_w)
    {
        winw = stage_w;
    }
    base_y = stage_y + 200;             /* room above for a drag upwards */
    /*
     * Two thirds of what is left below, not all of it. The stage is at most
     * 840 high, so all of it is always less than WINH and the window would
     * start out already touching the bottom of the stage - and then the
     * resize phase, which grows downwards from here, has nothing to grow
     * into: the bottom edge leg resizes to the size it already has.
     */
    if (winh > (stage_y + stage_h - base_y) * 2 / 3)
    {
        winh = (stage_y + stage_h - base_y) * 2 / 3;
    }
    base_x = stage_x + (stage_w - winw) / 2;

    wb = make_window (base_x + 80, stage_y + 40, "usagebench B");
    wa = make_window (base_x, base_y, "usagebench A");
    doc_buf = bw_canvas (winw, winh);

    /*
     * The drag carriers: no frame, and translucent the way a drag icon is, so
     * the compositor has to blend one as it moves. Told about their own map
     * and unmap, so a drag never starts before the last one has finished.
     */
    for (i = 0; i < 4; i++)
    {
        carrier[i] = bw_create (NULL, 0, 0, ICON + 2, ICON + 2, NULL,
                                BW_POPUP | BW_NOTIFY);
        bw_background_colour (carrier[i], 0xf0f0f0);
        bw_opacity (carrier[i], 0.75);
    }

    bw_sync ();
    sleep (2);

    /* Which way of moving a window this session honours. See lib/place.c.
       A managed Wayland toplevel takes no moves at all, only states and
       sizes, so there is nothing to probe against one. */
    if (bw_win_placed (wa))
    {
        int way = bench_probe_move (wa, base_x, base_y, winw, winh);

        if (manifest != NULL)
        {
            fprintf (manifest, "-- moves: %s\n",
                     bw_is_wayland ()
                   ? (way <= 0 ? "neither way works"
                    : bw_where_live (wa) ? "zone placement"
                                         : "layer-shell margins")
                   : (way == 0) ? "the pager request"
                   : (way == 1) ? "plain client calls"
                                : "neither way works");
            fflush (manifest);
        }
    }
    else if (manifest != NULL)
    {
        fprintf (manifest, "-- moves: states and client sizes (wayland)\n");
        fflush (manifest);
    }
    /*
     * The pattern becomes both windows' background before anything resizes
     * them. Every strip a window gains - maximised, snapped, dragged wider -
     * is then filled by the server from the pattern. Without it the new area
     * is undefined, and undefined is not the same thing twice: mutter left
     * the old pixels in the corner where xfwm4 and kwin left black.
     */
    bands_ready ();
    draw_content (wa, 0);
    draw_content (wb, 3 * BAND);
    bw_sync ();
    sleep (1);

    tasks = bench_tasks ();
    fixed = (tasks > 0 || ckdir != NULL);
    if (tasks > 0)
    {
        mark ("MEASURE-START");
    }
    start = bench_now ();
    run_start = start;
    run_seconds = seconds;
    do
    {
      if (want_phase ("windows"))
      {
        /* Maximize and back */
        for (i = 0; i < 2 && (fixed || bench_now () - start < seconds); i++)
        {
            bw_maximize (wa, 1);
            step ();
            expect_state (wa, BW_STATE_MAX, 1, "maximize");
            bw_maximize (wa, 0);
            step ();
            expect_state (wa, BW_STATE_MAX, 0, "unmaximize");
        }

        /* Minimize and back */
        for (i = 0; i < 2 && (fixed || bench_now () - start < seconds); i++)
        {
            bw_minimize (wa);
            step ();
            bw_restore (wa);
            bw_raise (wa);
            /* Politely, or GNOME posts a notification instead of focusing */
            bw_activate (wa);
            step ();
            expect_state (wa, BW_STATE_ACTIVE, 1, "the restore from minimize");
        }

        /* To the middle of each side edge, snapped to half the screen */
        walk_and_snap (wa, 0, sh / 2, 0, 0, sw / 2, sh);
        checkpoint ("side-left", 0, 0, sw / 2, sh);
        reset_window (wa, base_x, base_y);
        walk_and_snap (wa, sw, sh / 2, sw / 2, 0, sw / 2, sh);
        checkpoint ("side-right", sw / 2, 0, sw / 2, sh);
        reset_window (wa, base_x, base_y);

        /* Centred on each corner, a quarter each, clockwise then back */
        walk_and_snap (wa, 0, 0, 0, 0, sw / 2, sh / 2);
        checkpoint ("cw-tl", 0, 0, sw / 2, sh / 2);
        walk_and_snap (wa, sw, 0, sw / 2, 0, sw / 2, sh / 2);
        checkpoint ("cw-tr", sw / 2, 0, sw / 2, sh / 2);
        walk_and_snap (wa, sw, sh, sw / 2, sh / 2, sw / 2, sh / 2);
        checkpoint ("cw-br", sw / 2, sh / 2, sw / 2, sh / 2);
        walk_and_snap (wa, 0, sh, 0, sh / 2, sw / 2, sh / 2);
        checkpoint ("cw-bl", 0, sh / 2, sw / 2, sh / 2);
        reset_window (wa, base_x, base_y);
        walk_and_snap (wa, 0, sh, 0, sh / 2, sw / 2, sh / 2);
        checkpoint ("ccw-bl", 0, sh / 2, sw / 2, sh / 2);
        walk_and_snap (wa, sw, sh, sw / 2, sh / 2, sw / 2, sh / 2);
        checkpoint ("ccw-br", sw / 2, sh / 2, sw / 2, sh / 2);
        walk_and_snap (wa, sw, 0, sw / 2, 0, sw / 2, sh / 2);
        checkpoint ("ccw-tr", sw / 2, 0, sw / 2, sh / 2);
        walk_and_snap (wa, 0, 0, 0, 0, sw / 2, sh / 2);
        checkpoint ("ccw-tl", 0, 0, sw / 2, sh / 2);
        reset_window (wa, base_x, base_y);
      }

      if (want_phase ("scroll"))
      {
        /*
         * Scrolling, the two ways a person does it. The window is back at its
         * plain size here, so the document fills it.
         */
        doc_px = 0;
        draw_document (0, 0);

        /* One: the chevrons, eight clicks each way, three lines a click */
        for (i = 0; i < 8; i++)
        {
            doc_glide (doc_px + 3 * LINEH, 8, 0, 1);
            step ();
        }
        checkpoint ("scroll-chev-down", base_x, base_y, winw, winh);
        for (i = 0; i < 8; i++)
        {
            doc_glide (doc_px - 3 * LINEH, 8, 1, 0);
            step ();
        }
        checkpoint ("scroll-chev-up", base_x, base_y, winw, winh);

        /* Two: the thumb dragged the whole way down, then back */
        doc_glide (doc_max_px (), 120, 0, 0);
        step ();
        checkpoint ("scroll-thumb-down", base_x, base_y, winw, winh);
        doc_glide (0, 120, 0, 0);
        step ();
        checkpoint ("scroll-thumb-up", base_x, base_y, winw, winh);

        /* The bands again, so every later phase has content at any size */
        draw_content (wa, 0);
        bw_sync ();
      }

      if (want_phase ("resize"))
      {
        resize_phase (base_x, base_y);
      }

      if (want_phase ("dnd"))
      {
        dnd_phase (base_x, base_y);
      }

      if (want_phase ("windows"))
      {
        /* Two windows raised over each other, the way alt-tab lands */
        for (i = 0; i < 3 && (fixed || bench_now () - start < seconds); i++)
        {
            bw_raise (wb);
            step ();
            bw_raise (wa);
            step ();
        }

        /* Fullscreen and back */
        bw_fullscreen (wa, 1);
        step ();
        expect_state (wa, BW_STATE_FULLSCREEN, 1, "fullscreen");
        bw_fullscreen (wa, 0);
        step ();
        expect_state (wa, BW_STATE_FULLSCREEN, 0, "leaving fullscreen");
      }
        pass++;
    } while (tasks > 0 ? pass < tasks
                       : (ckdir == NULL && bench_now () - start < seconds));
    if (tasks > 0)
    {
        mark ("MEASURE-END");
    }

    printf ("AVERAGE %.1f actions/s over %.1f s, %d actions\n",
            actions / (bench_now () - start), bench_now () - start, actions);

    for (i = 0; i < 4; i++)
    {
        bw_destroy (carrier[i]);
    }
    if (bands_buf != NULL)
    {
        bw_destroy (bands_buf);
    }
    if (bands_buf_b != NULL)
    {
        bw_destroy (bands_buf_b);
    }
    int on_wayland = bw_is_wayland ();

    bw_destroy (doc_buf);
    bw_destroy (wa);
    bw_destroy (wb);
    bw_close ();

    /*
     * Snapping and drag and drop are both built out of moves. Where nothing
     * moves, the windows never took the shape the phase is about - the icons
     * were dragged into a window sitting underneath the one they left - so
     * the row is left empty rather than filled with a number for a scene
     * nobody designed. Resizing and scrolling do not move anything, and are
     * still worth measuring. On Wayland the same question is answered by the
     * compositor's configure events instead: a state asked for and never
     * granted is the window that sat still.
     */
    if (want_phase ("windows") &&
        (on_wayland ? state_wrong : !bench_moved ()))
    {
        printf ("MOVE-NEVER-HAPPENED the window stayed where it was\n");
        fflush (stdout);

        return 3;
    }
    if (want_phase ("dnd") && scene_wrong)
    {
        printf ("MOVE-NEVER-HAPPENED the two views never sat side by side\n");
        fflush (stdout);

        return 3;
    }

    return 0;
}
