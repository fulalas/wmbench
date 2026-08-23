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
 * Everything is plain X11 and EWMH, no input injection, so the run is the
 * same on every window manager and display server - which is the point: the
 * numbers are only comparable when every session did identical work.
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
 * capture.c, so it works on Wayland too). Two sessions' checkpoint folders
 * can then be compared pixel by pixel with compare_runs.sh: the content is
 * ours alone, so the desktop's own looks never enter the comparison.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "capture.h"
#include "polite.h"
#include "gate.h"
#include "stage.h"

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

static Display *d;
/*
 * The window size actually used, and the patch of screen everything is laid
 * out in: 1920x1080 or the screen if smaller, see stage.c. Maximising,
 * snapping to half the screen and going fullscreen are the window manager's
 * own geometry and use the real screen, sw and sh below.
 */
static int winw = WINW, winh = WINH;
static int stage_x, stage_y, stage_w, stage_h;
static Window root, wa, wb, carrier[4];
static GC gc;
static int scr, sw, sh, actions = 0;
static Pixmap doc_buf;          /* the document is drawn here, copied once */
static Pixmap bands_buf;        /* the bands, so a resize copies instead of drawing */
static Pixmap bands_buf_b;      /* the same for the second window, its own offset */
static const char *phase;       /* which part of the pass to run */
static int fixed;               /* a fixed number of passes, not a clock */
static int doc_px;              /* how far it is scrolled, in pixels */
static double pace;
static Atom net_state, state_max_h, state_max_v, state_fs, net_moveresize_atom;
static const char *ckdir;
static FILE *manifest;
static int cknum;

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

static double now (void)
{
    struct timespec ts;

    clock_gettime (CLOCK_MONOTONIC, &ts);

    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void step (void)
{
    XSync (d, False);
    actions++;
    usleep ((useconds_t) (pace * 1e6));
}

static void draw_content_size (Window w, int offset, int width, int height)
{
    int start = -(((offset % BAND) + BAND) % BAND);
    int j = 0, y;

    for (y = start; y < height; y += BAND, j++)
    {
        XSetForeground (d, gc,
                        palette[(((offset / BAND + j) % NCOL) + NCOL) % NCOL]);
        XFillRectangle (d, w, gc, 0, y, (unsigned) width, BAND);
    }
}

/* The whole screen's worth, so the window has content at any size */
static void draw_content (Window w, int offset)
{
    draw_content_size (w, offset, sw, sh);
}

/*
 * One checkpoint: what was asked, what the WM did, and what the window's own
 * content looks like, saved as a PPM cut to the asked size at the actual
 * position - identical content across sessions unless something differs.
 */
static void checkpoint_of (Window w, const char *name,
                           int ax, int ay, int aw, int ah)
{
    XWindowAttributes at;
    XImage *img;
    Window child;
    int rx, ry;

    if (ckdir == NULL)
    {
        return;
    }
    XSync (d, False);
    usleep (250000);            /* let the WM finish whatever it does */

    if (!XGetWindowAttributes (d, w, &at))
    {
        return;
    }
    XTranslateCoordinates (d, w, root, 0, 0, &rx, &ry, &child);
    fprintf (manifest, "%02d %-14s asked %d,%d %dx%d  got %d,%d %dx%d\n",
             cknum, name, ax, ay, aw, ah, rx, ry, at.width, at.height);
    fflush (manifest);

    /* Clamp to the screen; what falls outside cannot be photographed */
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (aw > sw - rx) aw = sw - rx;
    if (ah > sh - ry) ah = sh - ry;
    img = capture_region (d, root, rx, ry, (unsigned) aw, (unsigned) ah);
    if (img != NULL)
    {
        char path[512];
        FILE *f;
        int x, y;

        snprintf (path, sizeof path, "%s/ck-%02d-%s.ppm", ckdir, cknum, name);
        f = fopen (path, "wb");
        if (f != NULL)
        {
            fprintf (f, "P6\n%d %d\n255\n", img->width, img->height);
            for (y = 0; y < img->height; y++)
            {
                for (x = 0; x < img->width; x++)
                {
                    unsigned long v = XGetPixel (img, x, y);
                    unsigned char rgb[3];

                    rgb[0] = (v >> 16) & 0xff;
                    rgb[1] = (v >> 8) & 0xff;
                    rgb[2] = v & 0xff;
                    fwrite (rgb, 1, 3, f);
                }
            }
            fclose (f);
        }
        XDestroyImage (img);
    }
    cknum++;
}

/*
 * Move (and size, when tw is not 0) through _NET_MOVERESIZE_WINDOW, the
 * pager request: some compositors quietly ignore a plain client move of a
 * mapped window (mutter on Wayland does), but a pager is obeyed.
 */
static void checkpoint (const char *name, int ax, int ay, int aw, int ah)
{
    checkpoint_of (wa, name, ax, ay, aw, ah);
}

static int want_phase (const char *name)
{
    return strcmp (phase, "all") == 0 || strcmp (phase, name) == 0;
}

static int plain_moves;         /* the pager request did nothing here */

static void net_move (Window w, int x, int y, int tw, int th)
{
    XClientMessageEvent ev;

    /*
     * Some compositors ignore _NET_MOVERESIZE_WINDOW for X11 windows - labwc
     * does, and then nothing moves or resizes at all. Where the probe below
     * found that, the plain client call is used instead.
     */
    if (plain_moves)
    {
        if (tw > 0)
        {
            XMoveResizeWindow (d, w, x, y, (unsigned) tw, (unsigned) th);
        }
        else
        {
            XMoveWindow (d, w, x, y);
        }

        return;
    }

    memset (&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = net_moveresize_atom;
    ev.format = 32;
    /* StaticGravity, x and y valid, w and h too when given, from a pager */
    ev.data.l[0] = 10 | (3 << 8) | ((tw > 0 ? 12 : 0) << 8) | (2 << 12);
    ev.data.l[1] = x;
    ev.data.l[2] = y;
    ev.data.l[3] = tw;
    ev.data.l[4] = th;
    XSendEvent (d, root, False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &ev);
}

/* Ask the window manager, the way a pager or the window itself would */
static void set_state (Window w, int on, Atom a, Atom b)
{
    XClientMessageEvent ev;

    memset (&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = net_state;
    ev.format = 32;
    ev.data.l[0] = on ? 1 : 0;
    ev.data.l[1] = (long) a;
    ev.data.l[2] = (long) b;
    ev.data.l[3] = 1;
    XSendEvent (d, root, False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &ev);
}

/* Back to a known place and size, whatever the last phase left behind */
static void reset_window (Window w, int x, int y)
{
    set_state (w, 0, state_max_h, state_max_v);
    set_state (w, 0, state_fs, None);
    net_move (w, x, y, winw, winh);
    step ();
}

/*
 * Walk the window in steps, the way a drag carries it, until its centre sits
 * on the given point - the middle of an edge, or a corner - the way a person
 * drags to tile. Then snap it to the given rectangle, what edge or corner
 * tiling leaves behind. Plain moves, so it is identical under every WM.
 */
static void walk_and_snap (Window w, int cx, int cy,
                           int tx, int ty, int tw, int th)
{
    XWindowAttributes at;
    int x0, y0, wx, wy, i;
    Window child;

    XGetWindowAttributes (d, w, &at);
    XTranslateCoordinates (d, w, root, 0, 0, &x0, &y0, &child);
    wx = cx - at.width / 2;
    wy = cy - at.height / 2;
    for (i = 1; i <= 12; i++)
    {
        net_move (w, x0 + (wx - x0) * i / 12, y0 + (wy - y0) * i / 12, 0, 0);
        XSync (d, False);
        usleep (20000);
    }
    net_move (w, tx, ty, tw, th);
    step ();
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
    XRectangle clip = { 0, 0, winw - SBW, winh };
    XPoint tri[3];

    XSetForeground (d, gc, 0xffffff);
    XFillRectangle (d, doc_buf, gc, 0, 0, winw, winh);

    /* One line beyond each edge, clipped, so the edges cut cleanly */
    XSetClipRectangles (d, gc, 0, 0, &clip, 1, Unsorted);
    XSetForeground (d, gc, 0x202020);
    for (i = -1; i * LINEH - shift < winh; i++)
    {
        if (first + i < 0 || first + i >= DOCLINES)
        {
            continue;
        }
        y = 18 + i * LINEH - shift;
        line_text (first + i, line, sizeof line);
        XDrawString (d, doc_buf, gc, 12, y, line, (int) strlen (line));
    }
    XSetClipMask (d, gc, None);

    XSetForeground (d, gc, 0xd8d8d8);
    XFillRectangle (d, doc_buf, gc, winw - SBW, 0, SBW, winh);
    XSetForeground (d, gc, press_up ? 0x606060 : 0xb8b8b8);
    XFillRectangle (d, doc_buf, gc, winw - SBW, 0, SBW, CHEV);
    XSetForeground (d, gc, press_down ? 0x606060 : 0xb8b8b8);
    XFillRectangle (d, doc_buf, gc, winw - SBW, winh - CHEV, SBW, CHEV);
    XSetForeground (d, gc, 0x202020);
    tri[0].x = winw - SBW + 4;  tri[0].y = CHEV - 7;
    tri[1].x = winw - 4;        tri[1].y = CHEV - 7;
    tri[2].x = winw - SBW / 2;  tri[2].y = 6;
    XFillPolygon (d, doc_buf, gc, tri, 3, Convex, CoordModeOrigin);
    tri[0].x = winw - SBW + 4;  tri[0].y = winh - CHEV + 7;
    tri[1].x = winw - 4;        tri[1].y = winh - CHEV + 7;
    tri[2].x = winw - SBW / 2;  tri[2].y = winh - 6;
    XFillPolygon (d, doc_buf, gc, tri, 3, Convex, CoordModeOrigin);

    track = winh - 2 * CHEV;
    th = track * winh / (DOCLINES * LINEH);
    ty = CHEV + (track - th) * doc_px / (doc_max_px () ? doc_max_px () : 1);
    XSetForeground (d, gc, 0x707070);
    XFillRectangle (d, doc_buf, gc, winw - SBW + 3, ty, SBW - 6, th);

    XCopyArea (d, doc_buf, wa, gc, 0, 0, winw, winh, 0, 0);
    XSync (d, False);
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
    if (bands_buf != None)
    {
        return;
    }
    bands_buf = XCreatePixmap (d, wa, (unsigned) stage_w, (unsigned) stage_h,
                               (unsigned) DefaultDepth (d, scr));
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
    XSetWindowBackgroundPixmap (d, wa, bands_buf);

    /*
     * The second window needs one too. Without a background it keeps the black
     * it was created with, and nothing here listens for Expose, so every strip
     * of it that the resizing window uncovers turns black and stays black
     * until something else happens to paint it. That reads as a black edge
     * flashing along the window being resized, on every step.
     */
    if (bands_buf_b == None)
    {
        bands_buf_b = XCreatePixmap (d, wb, (unsigned) stage_w,
                                     (unsigned) stage_h,
                                     (unsigned) DefaultDepth (d, scr));
        draw_content_size (bands_buf_b, 3 * BAND, stage_w, stage_h);
    }
    XSetWindowBackgroundPixmap (d, wb, bands_buf_b);
    XSync (d, False);
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

    clock_gettime (CLOCK_MONOTONIC, &next);
    for (i = 1; i <= frames; i++)
    {
        x = x0 + (x1 - x0) * i / frames;
        y = y0 + (y1 - y0) * i / frames;
        w = w0 + (w1 - w0) * i / frames;
        h = h0 + (h1 - h0) * i / frames;
        net_move (wa, x, y, w, h);
        /*
         * Nothing is drawn here on purpose. The pattern is the window's
         * background, so the server fills every strip the window gains with
         * exactly what belongs there, at the moment it gains it. Anything this
         * program draws per step is drawn for a size the window may not have
         * yet, and a whole window redrawn while it is also moving tears.
         */
        XSync (d, False);
        pace_step (&next, 16000);
    }

    /*
     * Land exactly on the target. A stream of sixty requests can leave the
     * window manager a step behind, and then the checkpoint would record a
     * size nobody asked for.
     */
    net_move (wa, x1, y1, w1, h1);
    XSync (d, False);
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
        XWindowAttributes at;

        if (XGetWindowAttributes (d, wa, &at) &&
            (at.width == w0) && (at.height == h0) &&
            ((w1 != w0) || (h1 != h0)))
        {
            plain_moves = !plain_moves;
            net_move (wa, x1, y1, w1, h1);
            XSync (d, False);
            usleep (250000);
        }
    }
    XCopyArea (d, bands_buf, wa, gc, 0, 0, (unsigned) w1, (unsigned) h1, 0, 0);
    XSync (d, False);
}

/* The four handles a person grabs, each out and back, never off the screen */
static void resize_phase (int bx, int by)
{
    int gw = winw + 600, gh = winh + 400, tx, ty;

    bands_ready ();


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
 * for. StructureNotifyMask is selected on the carriers when they are made.
 */
static int wait_for (Window c, int type)
{
    XEvent ev;
    int i;

    for (i = 0; i < 200; i++)   /* up to a second, then carry on regardless */
    {
        if (XCheckTypedWindowEvent (d, c, type, &ev))
        {
            return 1;
        }
        XFlush (d);
        usleep (5000);
    }

    return 0;
}

/*
 * BENCH_DEBUG=1: say what the carrier really is at this moment - whether the
 * server has it mapped, where it sits, and how many big windows are stacked
 * above it. That is the difference between "not shown" and "shown behind
 * something", which look identical on screen.
 */
static void carrier_state (Window c, const char *when)
{
    XWindowAttributes a, b;
    Window par, r, *kids = NULL;
    unsigned int n, i, above = 0;
    int seen = 0;

    if (getenv ("BENCH_DEBUG") == NULL)
    {
        return;
    }
    if (!XGetWindowAttributes (d, c, &a))
    {
        printf ("  %-12s carrier is gone\n", when);
        fflush (stdout);

        return;
    }
    if (XQueryTree (d, root, &r, &par, &kids, &n))
    {
        for (i = 0; i < n; i++)
        {
            if (kids[i] == c)
            {
                seen = 1;
                continue;
            }
            if (seen && XGetWindowAttributes (d, kids[i], &b) &&
                b.map_state == IsViewable && b.width > 200)
            {
                above++;
            }
        }
        if (kids != NULL)
        {
            XFree (kids);
        }
    }
    printf ("  %-12s mapped %s at %d,%d, %u big windows above it\n", when,
            (a.map_state == IsViewable) ? "yes" : "no", a.x, a.y, above);
    fflush (stdout);
}

static void show_carrier (Window c, int x, int y)
{
    XMoveWindow (d, c, x, y);
    XMapRaised (d, c);
    if (!wait_for (c, MapNotify))
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
    XMoveWindow (d, c, x, y);
    XRaiseWindow (d, c);
    XSync (d, False);
    usleep (40000);             /* let the surface exist before it is drawn */
    carrier_state (c, "after map");
}

static void hide_carrier (Window c)
{
    const char *ms = getenv ("BENCH_DND_SETTLE");

    XUnmapWindow (d, c);
    if (!wait_for (c, UnmapNotify))
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
static void draw_icon (Drawable t, int x, int y, int i)
{
    XPoint fold[3];

    XSetForeground (d, gc, palette[i % NCOL]);
    XFillRectangle (d, t, gc, x, y, ICON, ICON);
    XSetForeground (d, gc, 0xffffff);
    fold[0].x = x + ICON - 22; fold[0].y = y;
    fold[1].x = x + ICON;      fold[1].y = y;
    fold[2].x = x + ICON;      fold[2].y = y + 22;
    XFillPolygon (d, t, gc, fold, 3, Convex, CoordModeOrigin);
    XSetForeground (d, gc, 0x303030);
    XDrawRectangle (d, t, gc, x, y, ICON, ICON);
    XFillRectangle (d, t, gc, x + 8, y + ICON + 8, ICON - 16, 6);
}

/* The two icon views: what is still here, what has been dropped, and whether
 * a drag is hovering over this one */
static void draw_view (Window w, const char *title, const int *have,
                       int count, int hot)
{
    int i, cx, cy;

    XSetForeground (d, gc, 0xf0f0f0);
    XFillRectangle (d, w, gc, 0, 0, winw, winh);
    XSetForeground (d, gc, hot ? 0x3070d0 : 0xc0c0c0);
    XDrawRectangle (d, w, gc, 24, 48, winw - 48, winh - 72);
    XDrawRectangle (d, w, gc, 25, 49, winw - 50, winh - 74);
    if (hot)
    {
        XDrawRectangle (d, w, gc, 26, 50, winw - 52, winh - 76);
    }
    XSetForeground (d, gc, 0x404040);
    XDrawString (d, w, gc, 40, 36, title, (int) strlen (title));

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
            XSetForeground (d, gc, 0xb0b0b0);
            XDrawRectangle (d, w, gc, cx, cy, ICON, ICON);
        }
    }
    XSync (d, False);
}

/*
 * Icons dragged from one window into another. What the compositor sees is a
 * small frameless translucent window travelling over another window, which is
 * exactly what a drag is; the last leg carries four of them at once.
 */
static void dnd_phase (int bx, int by)
{
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

    net_move (wa, lx, by, winw, winh);
    net_move (wb, rx, by, winw, winh);
    XSync (d, False);
    usleep (300000);
    draw_view (wa, "Pictures", home, NICON, 0);
    draw_view (wb, "Album", album, 0, 0);
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
    show_carrier (carrier[0], lx + cx, by + cy);
    draw_icon (carrier[0], 1, 1, order[0]);
    XSync (d, False);

    for (k = 0; k < 4; k++)
    {
        i = order[k];
        icon_cell (i, &cx, &cy);
        icon_cell (nalbum, &tx, &ty);

        /* Lift: the slot empties and what was over it is now being carried */
        if (getenv ("BENCH_DEBUG") != NULL)
        {
            printf ("drag %d\n", k + 1);
            fflush (stdout);
        }
        home[i] = -1;
        draw_view (wa, "Pictures", home, NICON, 0);
        XSync (d, False);
        usleep (150000);
        carrier_state (carrier[0], "at lift");
        snprintf (ckname, sizeof ckname, "dnd-lift-%d", k + 1);
        checkpoint_of (wa, ckname, lx, by, winw, winh);

        /* Carried across in an arc, the way a hand moves */
        for (f = 1; f <= 55; f++)
        {
            double t = (double) f / 55.0;

            x = (int) ((lx + cx) + ((rx + tx) - (lx + cx)) * t);
            y = (int) ((by + cy) + ((by + ty) - (by + cy)) * t
                       - 90.0 * t * (1.0 - t) * 4.0);
            XMoveWindow (d, carrier[0], x, y);
            draw_icon (carrier[0], 1, 1, i);
            if (f == 28)
            {
                draw_view (wb, "Album", album, nalbum, 1);
                carrier_state (carrier[0], "mid flight");
            }
            XSync (d, False);
            usleep (16000);
        }

        /* Dropped: the album has it now */
        album[nalbum++] = i;
        draw_view (wb, "Album", album, nalbum, 0);

        /*
         * And the carrier moves straight on to whatever it picks up next -
         * the next icon in the source, or the album's first icon when the
         * single drags are done and all four are about to come back.
         */
        if (k + 1 < 4)
        {
            icon_cell (order[k + 1], &cx, &cy);
            XMoveWindow (d, carrier[0], lx + cx, by + cy);
            draw_icon (carrier[0], 1, 1, order[k + 1]);
        }
        else
        {
            icon_cell (0, &cx, &cy);
            XMoveWindow (d, carrier[0], rx + cx, by + cy);
            draw_icon (carrier[0], 1, 1, order[0]);
        }
        XSync (d, False);
        step ();
        snprintf (ckname, sizeof ckname, "dnd-drop-%d", k + 1);
        checkpoint_of (wb, ckname, rx, by, winw, winh);
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
        XSync (d, False);
        usleep (200000);

        /* The album lets go of all four, and they are all being carried */
        nalbum = 0;
        for (i = 0; i < NICON; i++)
        {
            album[i] = -1;
        }
        draw_view (wb, "Album", album, 0, 0);
        for (k = 0; k < 4; k++)
        {
            icon_cell (k, &cx, &cy);
            XMoveWindow (d, carrier[k], ax + (rx + cx - ax), ay + (by + cy - ay));
            draw_icon (carrier[k], 1, 1, order[k]);
        }
        XSync (d, False);
        usleep (300000);

        for (f = 1; f <= 55; f++)
        {
            double t = (double) f / 55.0;

            x = (int) (ax + (hx - ax) * t);
            y = (int) (ay + (hy - ay) * t - 90.0 * t * (1.0 - t) * 4.0);
            for (k = 0; k < 4; k++)
            {
                XMoveWindow (d, carrier[k], x + k * 14, y + k * 10);
                draw_icon (carrier[k], 1, 1, order[k]);
            }
            if (f == 28)
            {
                draw_view (wa, "Pictures", home, NICON, 1);
            }
            XSync (d, False);
            usleep (16000);
        }

        /* Home: the icons are back in their slots and the carriers are done */
        for (k = 0; k < 4; k++)
        {
            home[order[k]] = order[k];
        }
        draw_view (wa, "Pictures", home, NICON, 0);
        XSync (d, False);
        for (k = 0; k < 4; k++)
        {
            hide_carrier (carrier[k]);
        }
        step ();
        checkpoint_of (wa, "dnd-all-back", lx, by, winw, winh);
    }

    /* Put the two windows back at the width and place the other phases expect */
    winw = wide;
    net_move (wb, stage_x + (stage_w - winw) / 2 + 80, stage_y + 40,
              winw, winh);
    net_move (wa, bx, by, winw, winh);
    draw_content (wa, 0);
    draw_content (wb, 3 * BAND);
    XSync (d, False);
    step ();
}

static Window make_window (int x, int y, const char *name)
{
    Window w;
    XSizeHints hints;
    Atom wtype, normal;

    w = XCreateSimpleWindow (d, root, x, y, winw, winh, 0,
                             BlackPixel (d, scr), BlackPixel (d, scr));
    /*
     * Cleared first. Only the flags below are set, but the rest of the struct
     * still travels to the window manager, and whatever the stack happened to
     * hold reads as a minimum size, a maximum size, a size step or an aspect
     * ratio. One window manager refused this window's resizes because of
     * it: asked for 1400 wide at x=970, it granted 1110 at x=1265, so a
     * resize leg ran into a wall part way through.
     */
    memset (&hints, 0, sizeof hints);
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = x; hints.y = y; hints.width = winw; hints.height = winh;
    /*
     * Keep the pixels on a resize. The default is ForgetGravity, where the
     * server throws the window's contents away every time its size changes
     * and tiles it from the background again - a full window repaint per step
     * of a resize, which flickers whether anything composites or not. With
     * this, what is already drawn stays and only the strip the window gained
     * has to be filled.
     */
    {
        XSetWindowAttributes at;

        at.bit_gravity = NorthWestGravity;
        XChangeWindowAttributes (d, w, CWBitGravity, &at);
    }

    XSetWMNormalHints (d, w, &hints);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, w, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, w, name);
    XSelectInput (d, w, StructureNotifyMask);
    XMapWindow (d, w);

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

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);
    sw = DisplayWidth (d, scr);
    sh = DisplayHeight (d, scr);
    /*
     * Everything this program places goes inside the stage, so it fits a
     * 1080p screen and does the same amount of work on any screen.
     */
    bench_stage (d, SAFE, &stage_x, &stage_y, &stage_w, &stage_h);
    if (winw > stage_w)
    {
        winw = stage_w;
    }
    base_y = stage_y + 200;             /* room above for a drag upwards */
    if (winh > stage_y + stage_h - base_y)
    {
        winh = stage_y + stage_h - base_y;
    }
    base_x = stage_x + (stage_w - winw) / 2;
    net_state = XInternAtom (d, "_NET_WM_STATE", False);
    state_max_h = XInternAtom (d, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    state_max_v = XInternAtom (d, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    state_fs = XInternAtom (d, "_NET_WM_STATE_FULLSCREEN", False);
    net_moveresize_atom = XInternAtom (d, "_NET_MOVERESIZE_WINDOW", False);

    wb = make_window (base_x + 80, stage_y + 40, "usagebench B");
    wa = make_window (base_x, base_y, "usagebench A");
    gc = XCreateGC (d, wa, 0, NULL);
    doc_buf = XCreatePixmap (d, wa, winw, winh,
                             (unsigned) DefaultDepth (d, scr));

    /*
     * The drag carriers: no frame, and translucent the way a drag icon is, so
     * the compositor has to blend one as it moves. Told about their own map
     * and unmap, so a drag never starts before the last one has finished.
     */
    {
        XSetWindowAttributes swa;
        unsigned long opacity = (unsigned long) (0.75 * 0xffffffffUL);

        memset (&swa, 0, sizeof swa);
        swa.override_redirect = True;
        swa.background_pixel = 0xf0f0f0;
        for (i = 0; i < 4; i++)
        {
            carrier[i] = XCreateWindow (d, root, 0, 0, ICON + 2, ICON + 2, 0,
                                        CopyFromParent, InputOutput,
                                        CopyFromParent,
                                        CWOverrideRedirect | CWBackPixel, &swa);
            XSelectInput (d, carrier[i], StructureNotifyMask);
            XChangeProperty (d, carrier[i],
                             XInternAtom (d, "_NET_WM_WINDOW_OPACITY", False),
                             XA_CARDINAL, 32, PropModeReplace,
                             (unsigned char *) &opacity, 1);
        }
    }

    /*
     * Which way of moving a window this session honours: ask the pager way,
     * then look. A frame can offset the answer, so only a wild miss counts.
     */
    {
        int px, py, off = 60;
        Window pch;
        XWindowAttributes pat;

        /*
         * Ask for a different place and a different size, then look at both.
         * Smaller, not bigger: bigger would step outside the stage for as
         * long as the probe lasts.
         */
        net_move (wa, base_x + off, base_y + off, winw - off, winh - off);
        XSync (d, False);
        usleep (500000);
        XTranslateCoordinates (d, wa, root, 0, 0, &px, &py, &pch);
        XGetWindowAttributes (d, wa, &pat);
        if (px < base_x + off - 80 || px > base_x + off + 80 ||
            py < base_y + off - 80 || py > base_y + off + 80 ||
            pat.width != winw - off || pat.height != winh - off)
        {
            plain_moves = 1;
        }
        net_move (wa, base_x, base_y, winw, winh);
        XSync (d, False);
        usleep (300000);
        printf ("moves: %s\n", plain_moves ? "plain client calls"
                                            : "the pager request");
        fflush (stdout);
        if (manifest != NULL)
        {
            fprintf (manifest, "-- moves: %s\n",
                     plain_moves ? "plain client calls" : "the pager request");
            fflush (manifest);
        }
    }
    XSync (d, False);
    sleep (2);
    draw_content (wa, 0);
    draw_content (wb, 3 * BAND);
    XSync (d, False);
    sleep (1);

    tasks = bench_tasks ();
    fixed = (tasks > 0 || ckdir != NULL);
    if (tasks > 0)
    {
        mark ("MEASURE-START");
    }
    start = now ();
    do
    {
      if (want_phase ("windows"))
      {
        /* Maximize and back */
        for (i = 0; i < 2 && (fixed || now () - start < seconds); i++)
        {
            set_state (wa, 1, state_max_h, state_max_v);
            step ();
            set_state (wa, 0, state_max_h, state_max_v);
            step ();
        }

        /* Minimize and back */
        for (i = 0; i < 2 && (fixed || now () - start < seconds); i++)
        {
            XIconifyWindow (d, wa, scr);
            step ();
            XMapRaised (d, wa);
            /* Politely, or GNOME posts a notification instead of focusing */
            polite_activate (d, root, wa);
            step ();
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
        XSync (d, False);
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
        for (i = 0; i < 3 && (fixed || now () - start < seconds); i++)
        {
            XRaiseWindow (d, wb);
            step ();
            XRaiseWindow (d, wa);
            step ();
        }

        /* Fullscreen and back */
        set_state (wa, 1, state_fs, None);
        step ();
        set_state (wa, 0, state_fs, None);
        step ();
      }
        pass++;
    } while (tasks > 0 ? pass < tasks
                       : (ckdir == NULL && now () - start < seconds));
    if (tasks > 0)
    {
        mark ("MEASURE-END");
    }

    printf ("AVERAGE %.1f actions/s over %.1f s, %d actions\n",
            actions / (now () - start), now () - start, actions);

    for (i = 0; i < 4; i++)
    {
        XDestroyWindow (d, carrier[i]);
    }
    if (bands_buf != None)
    {
        XFreePixmap (d, bands_buf);
    }
    XFreePixmap (d, doc_buf);
    XDestroyWindow (d, wa);
    XDestroyWindow (d, wb);
    XCloseDisplay (d);

    return 0;
}
