/* The bodies here are the suite's original Xlib code, moved behind the bw_
   calls unchanged, so an X11 run measures what it measured before the split */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XShm.h>
#include "win_priv.h"

typedef struct {
    Window xid;
    Pixmap pix;                 /* when the bw_win is a canvas */
    GC gc;
    Colormap cmap;
    XShmSegmentInfo shm;
    XImage *frame;
    int frame_failed;
} x11_win;

static Display *d;
static int scr;
static Window root;

/* Interned once: pager_move runs at 120 Hz and x11_state interns five a
   call, and every one of those takes Xlib's display lock */
static Atom a_time, a_active, a_state, a_max_h, a_max_v, a_fs, a_focused;
static Atom a_moveresize, a_above, a_wtype, a_normal, a_opacity, a_opaque;

static int x11_open (void)
{
    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        return 0;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    a_time = XInternAtom (d, "_BENCH_TIME", False);
    a_active = XInternAtom (d, "_NET_ACTIVE_WINDOW", False);
    a_state = XInternAtom (d, "_NET_WM_STATE", False);
    a_max_h = XInternAtom (d, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    a_max_v = XInternAtom (d, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    a_fs = XInternAtom (d, "_NET_WM_STATE_FULLSCREEN", False);
    a_focused = XInternAtom (d, "_NET_WM_STATE_FOCUSED", False);
    a_moveresize = XInternAtom (d, "_NET_MOVERESIZE_WINDOW", False);
    a_above = XInternAtom (d, "_NET_WM_STATE_ABOVE", False);
    a_wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    a_normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    a_opacity = XInternAtom (d, "_NET_WM_WINDOW_OPACITY", False);
    a_opaque = XInternAtom (d, "_NET_WM_OPAQUE_REGION", False);

    return 1;
}

static void x11_close (void)
{
    XCloseDisplay (d);
    d = NULL;
}

static void x11_screen_size (int *w, int *h)
{
    if (w != NULL) *w = DisplayWidth (d, scr);
    if (h != NULL) *h = DisplayHeight (d, scr);
}

#define STAGE_W 1920
#define STAGE_H 1080

/* Xinerama's answer for one output, laid out as it has been since 1998 */
typedef struct {
    int screen_number;
    short x_org, y_org, width, height;
} output_info;

/*
 * Where the primary monitor is.
 *
 * DisplayWidth and DisplayHeight give the union of every output, so on two
 * 1920x1080 monitors side by side the middle of the screen is the seam
 * between them: a stage centred there is split down the middle, composited
 * and presented twice, and the number cannot be put beside a single-monitor
 * one. Xinerama knows where each output really is, and the server lists the
 * primary one first.
 *
 * It is opened by hand rather than linked so that every program drawing a
 * stage does not gain a library it needs only here, and so that a desktop
 * without Xinerama simply keeps the whole-screen answer. The handle is never
 * closed: the library leaves a hook behind in the Display that XCloseDisplay
 * would call after it had gone.
 */
static int primary_monitor (int *x, int *y, int *w, int *h)
{
    static output_info *(*query) (Display *, int *);
    static int looked;
    output_info *out;
    int n = 0, ok = 0;

    if (!looked)
    {
        void *lib = dlopen ("libXinerama.so.1", RTLD_LAZY);

        looked = 1;
        if (lib != NULL)
        {
            query = (output_info *(*) (Display *, int *))
                    dlsym (lib, "XineramaQueryScreens");
        }
    }
    if (query == NULL)
    {
        return 0;
    }
    out = query (d, &n);
    if (out == NULL)
    {
        return 0;
    }
    if (n > 0 && out[0].width > 0 && out[0].height > 0)
    {
        *x = out[0].x_org;
        *y = out[0].y_org;
        *w = out[0].width;
        *h = out[0].height;
        ok = 1;
    }
    XFree (out);

    return ok;
}

static void x11_stage (int margin, int *x, int *y, int *w, int *h)
{
    int screen_x = 0, screen_y = 0;
    int screen_w = DisplayWidth (d, scr);
    int screen_h = DisplayHeight (d, scr);
    int stage_w, stage_h;

    primary_monitor (&screen_x, &screen_y, &screen_w, &screen_h);
    stage_w = (screen_w < STAGE_W) ? screen_w : STAGE_W;
    stage_h = (screen_h < STAGE_H) ? screen_h : STAGE_H;

    *x = screen_x + (screen_w - stage_w) / 2 + margin;
    *y = screen_y + (screen_h - stage_h) / 2 + margin;
    *w = stage_w - 2 * margin;
    *h = stage_h - 2 * margin;
    if (*w < 1)
    {
        *w = 1;
    }
    if (*h < 1)
    {
        *h = 1;
    }
}

/* The server's current time, fished out of a PropertyNotify on the window */
static Time server_time (Window w)
{
    XWindowAttributes at;
    XEvent ev;
    Atom a = a_time;
    long mask = 0;

    if (XGetWindowAttributes (d, w, &at))
    {
        mask = at.your_event_mask;
    }
    XSelectInput (d, w, mask | PropertyChangeMask);
    XChangeProperty (d, w, a, XA_STRING, 8, PropModeAppend,
                     (unsigned char *) "", 0);
    XSync (d, False);
    /*
     * Only this window's PropertyNotify. Taking the queue apart with
     * XNextEvent would throw away every other event waiting on the same
     * connection, and a caller that is sitting on the queue for a map or an
     * unmap of its own would never see it - which is how a carrier window
     * once stayed invisible for a whole run.
     */
    while (XCheckTypedWindowEvent (d, w, PropertyNotify, &ev))
    {
        if (ev.xproperty.atom == a)
        {
            XSelectInput (d, w, mask);

            return ev.xproperty.time;
        }
    }
    XSelectInput (d, w, mask);

    return CurrentTime;
}

/*
 * Raising or focusing a window without a real user timestamp reads as focus
 * stealing, and GNOME answers with a "window is ready" notification instead
 * of doing it. This asks the way a pager does, with the server's own time.
 */
static void x11_activate (bw_win *win)
{
    x11_win *xw = win->impl;
    XClientMessageEvent ev;

    memset (&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = xw->xid;
    ev.message_type = a_active;
    ev.format = 32;
    ev.data.l[0] = 2;                   /* a pager asks */
    ev.data.l[1] = (long) server_time (xw->xid);
    XSendEvent (d, root, False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &ev);
    XSync (d, False);
}

/*
 * Ask to be kept above the other windows, before the window is mapped: as an
 * initial state the window manager reads it while framing, and one raise
 * afterwards would not survive a load that keeps raising its own windows.
 */
static void keep_above (Window w)
{
    Atom above = a_above;

    XChangeProperty (d, w, a_state,
                     XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &above, 1);
}

static int x11_create (bw_win *win)
{
    x11_win *xw = calloc (1, sizeof *xw);
    XSetWindowAttributes swa;
    unsigned long swa_mask = 0;
    int unmanaged = (win->flags & (BW_POPUP | BW_UNMANAGED)) != 0;

    if (xw == NULL)
    {
        return 0;
    }
    memset (&swa, 0, sizeof swa);

    if (win->flags & BW_ARGB)
    {
        /* A 32-bit visual, the way a toolkit doing its own decorations asks */
        XVisualInfo tmpl, *vi;
        int nvi;

        tmpl.screen = scr;
        tmpl.depth = 32;
        tmpl.class = TrueColor;
        vi = XGetVisualInfo (d, VisualScreenMask | VisualDepthMask |
                             VisualClassMask, &tmpl, &nvi);
        if (vi == NULL)
        {
            free (xw);

            return 0;
        }
        /* A list that came back empty is still a list, and still has to go */
        if (nvi == 0)
        {
            XFree (vi);
            free (xw);

            return 0;
        }
        xw->cmap = XCreateColormap (d, root, vi->visual, AllocNone);
        swa.colormap = xw->cmap;
        swa.border_pixel = 0;
        swa.background_pixel = 0;
        swa.override_redirect = unmanaged;
        xw->xid = XCreateWindow (d, root, win->x, win->y,
                                 (unsigned) win->w, (unsigned) win->h, 0, 32,
                                 InputOutput, vi->visual,
                                 CWColormap | CWBorderPixel | CWBackPixel |
                                 CWOverrideRedirect, &swa);
        XFree (vi);
    }
    else if (unmanaged)
    {
        /*
         * Override redirect, the way a menu really is: no frame, no manager.
         * The background is set as well, so a strip the window gains has
         * defined contents - resize_check hunts undefined pixels at a
         * growing edge, and without this it would find its own.
         */
        swa.override_redirect = True;
        swa.background_pixel = BlackPixel (d, scr);
        swa_mask = CWOverrideRedirect | CWBackPixel;
        xw->xid = XCreateWindow (d, root, win->x, win->y,
                                 (unsigned) win->w, (unsigned) win->h, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 swa_mask, &swa);
    }
    else
    {
        xw->xid = XCreateSimpleWindow (d, root, win->x, win->y,
                                       (unsigned) win->w, (unsigned) win->h, 0,
                                       BlackPixel (d, scr), BlackPixel (d, scr));
    }

    if ((win->flags & BW_PLACED) && !(win->flags & BW_LOOSE))
    {
        /*
         * Cleared first: only the flags below are set, but the whole struct
         * still travels to the window manager, and whatever the stack held
         * reads as a minimum size, a size step or an aspect ratio.
         */
        XSizeHints hints;

        memset (&hints, 0, sizeof hints);
        hints.flags = USPosition | USSize | PPosition | PSize;
        hints.x = win->x; hints.y = win->y;
        hints.width = win->w; hints.height = win->h;
        if (win->flags & BW_FIXED)
        {
            hints.flags |= PMinSize | PMaxSize;
            hints.min_width = hints.max_width = win->w;
            hints.min_height = hints.max_height = win->h;
        }
        XSetWMNormalHints (d, xw->xid, &hints);
    }
    if (win->flags & BW_KEEP)
    {
        /*
         * Keep the pixels on a resize. The default is ForgetGravity, where
         * the server throws the window's contents away every time its size
         * changes and tiles it from the background again - a full window
         * repaint per step of a resize, which flickers whether anything
         * composites or not. With this, what is already drawn stays and only
         * the strip the window gained has to be filled.
         */
        XSetWindowAttributes at;

        at.bit_gravity = NorthWestGravity;
        XChangeWindowAttributes (d, xw->xid, CWBitGravity, &at);
    }
    if (!unmanaged)
    {
        Atom wtype = a_wtype, normal = a_normal;
        /*
         * Focusable, said out loud. Activation only acts on the window that
         * can take the focus, and without the input hint one manager never
         * gave it - and then suspend_check passed without testing anything.
         */
        XWMHints wm;

        wm.flags = InputHint | StateHint;
        wm.input = True;
        wm.initial_state = NormalState;
        XSetWMHints (d, xw->xid, &wm);
        XChangeProperty (d, xw->xid, wtype, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *) &normal, 1);
    }
    if (win->name[0] != '\0')
    {
        XStoreName (d, xw->xid, win->name);
    }
    if (win->flags & BW_ABOVE)
    {
        keep_above (xw->xid);
    }
    if (win->flags & BW_NOTIFY)
    {
        XSelectInput (d, xw->xid, StructureNotifyMask);
    }
    xw->gc = XCreateGC (d, xw->xid, 0, NULL);
    win->impl = xw;

    return 1;
}

static int x11_canvas_new (bw_win *win)
{
    x11_win *xw = calloc (1, sizeof *xw);

    if (xw == NULL)
    {
        return 0;
    }
    xw->pix = XCreatePixmap (d, root, (unsigned) win->w, (unsigned) win->h,
                             (unsigned) DefaultDepth (d, scr));
    xw->gc = XCreateGC (d, xw->pix, 0, NULL);
    win->impl = xw;

    return 1;
}

/*
 * The shared image is made once, at the size the window had then. A resize
 * invalidates it: x11_frame_size reports the new size and the caller fills
 * that many pixels, which in a segment made for the old one runs off the end.
 * So it is dropped and made again rather than kept.
 */
static void frame_drop (x11_win *xw)
{
    if (xw->frame == NULL)
    {
        return;
    }
    XShmDetach (d, &xw->shm);
    XDestroyImage (xw->frame);
    shmdt (xw->shm.shmaddr);
    memset (&xw->shm, 0, sizeof xw->shm);
    xw->frame = NULL;
}

static void x11_destroy (bw_win *win)
{
    x11_win *xw = win->impl;

    frame_drop (xw);
    XFreeGC (d, xw->gc);
    if (win->canvas)
    {
        XFreePixmap (d, xw->pix);
    }
    else
    {
        XDestroyWindow (d, xw->xid);
    }
    if (xw->cmap != None)
    {
        XFreeColormap (d, xw->cmap);
    }
    free (xw);
}

static void x11_map (bw_win *win)
{
    XMapWindow (d, ((x11_win *) win->impl)->xid);
}

static void x11_unmap (bw_win *win)
{
    XUnmapWindow (d, ((x11_win *) win->impl)->xid);
}

/*
 * A map or an unmap is a request, not a fact: asking and then carrying on
 * assumes the pair is acted on in order, and under XWayland it is not, which
 * left a carrier invisible and made the icon look like it teleported. So each
 * one can be waited for. BW_NOTIFY selects the events this reads.
 */
static int x11_wait_shown (bw_win *win, int shown)
{
    x11_win *xw = win->impl;
    XEvent ev;
    int i;

    for (i = 0; i < 200; i++)   /* up to a second, then carry on regardless */
    {
        if (XCheckTypedWindowEvent (d, xw->xid,
                                    shown ? MapNotify : UnmapNotify, &ev))
        {
            return 1;
        }
        XFlush (d);
        usleep (5000);
    }

    return 0;
}

static void x11_raise (bw_win *win)
{
    XRaiseWindow (d, ((x11_win *) win->impl)->xid);
}

static void x11_restore (bw_win *win)
{
    XMapWindow (d, ((x11_win *) win->impl)->xid);
}

/*
 * The focus taken directly, on top of asking the manager politely. A
 * compositor suspends only for the window that holds the focus, and a
 * manager that ignores the pager request would leave suspend_check testing
 * nothing at all. Wayland has no such call - the compositor decides - and
 * says so by doing nothing.
 */
static void x11_take_focus (bw_win *win)
{
    XSetInputFocus (d, ((x11_win *) win->impl)->xid, RevertToPointerRoot,
                    CurrentTime);
    XSync (d, False);
}

static int x11_win_placed (bw_win *win)
{
    (void) win;

    /* X11 grants a position or is caught not to; either way it can be asked */
    return 1;
}

static int x11_win_aimable (bw_win *win)
{
    (void) win;

    return 1;
}

/* The pager request, the one a taskbar sends */
static void pager_move (Window w, int x, int y, int width, int height)
{
    XClientMessageEvent mev;

    memset (&mev, 0, sizeof mev);
    mev.type = ClientMessage;
    mev.window = w;
    mev.message_type = a_moveresize;
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

static int x11_move_raw (bw_win *win, int x, int y, int width, int height,
                         int way)
{
    Window w = ((x11_win *) win->impl)->xid;

    if (way)
    {
        /*
         * Both sides, not just the width: a zero height is a BadValue and a
         * negative one becomes an enormous unsigned, and either way Xlib's
         * default error handler ends the program in the middle of a run.
         */
        if (width > 0 && height > 0)
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
        pager_move (w, x, y, width, height);
    }

    return 0;
}

static int x11_where_live (bw_win *win)
{
    (void) win;

    return 1;
}

static void x11_where (bw_win *win, int *x, int *y, int *width, int *height)
{
    x11_win *xw = win->impl;
    XWindowAttributes at;
    Window child;
    int rx = 0, ry = 0;

    if (!XGetWindowAttributes (d, xw->xid, &at))
    {
        at.width = at.height = 0;
    }
    XTranslateCoordinates (d, xw->xid, root, 0, 0, &rx, &ry, &child);
    if (x != NULL) *x = rx;
    if (y != NULL) *y = ry;
    if (width != NULL) *width = at.width;
    if (height != NULL) *height = at.height;
}

static void x11_resize (bw_win *win, int width, int height)
{
    XResizeWindow (d, ((x11_win *) win->impl)->xid,
                   (unsigned) width, (unsigned) height);
}

/* Maximise, minimise, fullscreen: the window manager's own states */
static void set_state (Window w, int on, Atom a, Atom b)
{
    XClientMessageEvent ev;

    memset (&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = a_state;
    ev.format = 32;
    ev.data.l[0] = on ? 1 : 0;
    ev.data.l[1] = (long) a;
    ev.data.l[2] = (long) b;
    ev.data.l[3] = 1;
    XSendEvent (d, root, False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &ev);
}

static void x11_maximize (bw_win *win, int on)
{
    set_state (((x11_win *) win->impl)->xid, on,
               a_max_h, a_max_v);
}

static void x11_minimize (bw_win *win)
{
    XIconifyWindow (d, ((x11_win *) win->impl)->xid, scr);
}

static void x11_fullscreen (bw_win *win, int on)
{
    set_state (((x11_win *) win->impl)->xid, on,
               a_fs, None);
    XFlush (d);
}

static unsigned x11_state (bw_win *win)
{
    x11_win *xw = win->impl;
    Atom type = None, *st;
    int fmt;
    unsigned long n = 0, rest, i;
    unsigned char *p = NULL;
    unsigned out = 0;
    Atom max_h = a_max_h, max_v = a_max_v, fs = a_fs, focused = a_focused;
    int got_h = 0, got_v = 0;

    if (XGetWindowProperty (d, xw->xid,
                            a_state, 0, 32,
                            False, XA_ATOM, &type, &fmt, &n, &rest,
                            &p) == Success && p != NULL)
    {
        st = (Atom *) p;
        for (i = 0; i < n; i++)
        {
            if (st[i] == max_h) got_h = 1;
            if (st[i] == max_v) got_v = 1;
            if (st[i] == fs) out |= BW_STATE_FULLSCREEN;
            if (st[i] == focused) out |= BW_STATE_ACTIVE;
        }
        XFree (p);
    }
    if (got_h && got_v)
    {
        out |= BW_STATE_MAX;
    }

    return out;
}

static int x11_opacity (bw_win *win, double alpha)
{
    unsigned long opacity;

    /* A 32-bit CARDINAL: out of range it would wrap rather than saturate */
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    opacity = (unsigned long) (alpha * 0xffffffffUL);
    /* The window manager reads this and tells the compositor to blend */
    XChangeProperty (d, ((x11_win *) win->impl)->xid,
                     a_opacity,
                     XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *) &opacity, 1);

    return 1;
}

static void x11_opaque_region (bw_win *win, int x, int y, int width, int height)
{
    long opaque[4];

    opaque[0] = x;
    opaque[1] = y;
    opaque[2] = width;
    opaque[3] = height;
    XChangeProperty (d, ((x11_win *) win->impl)->xid,
                     a_opaque,
                     XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *) opaque, 4);
}

static void x11_background_colour (bw_win *win, unsigned long colour)
{
    XSetWindowBackground (d, ((x11_win *) win->impl)->xid, colour);
}

static void x11_set_background (bw_win *win, bw_win *canvas)
{
    XSetWindowBackgroundPixmap (d, ((x11_win *) win->impl)->xid,
                                ((x11_win *) canvas->impl)->pix);
}

static Drawable target_of (bw_win *win)
{
    x11_win *xw = win->impl;

    return win->canvas ? (Drawable) xw->pix : (Drawable) xw->xid;
}

static void x11_fill (bw_win *win, unsigned long c, int x, int y,
                      int width, int height)
{
    x11_win *xw = win->impl;

    XSetForeground (d, xw->gc, c);
    XFillRectangle (d, target_of (win), xw->gc, x, y,
                    (unsigned) width, (unsigned) height);
}

static void x11_rect (bw_win *win, unsigned long c, int x, int y,
                      int width, int height)
{
    x11_win *xw = win->impl;

    XSetForeground (d, xw->gc, c);
    XDrawRectangle (d, target_of (win), xw->gc, x, y,
                    (unsigned) width, (unsigned) height);
}

static void x11_poly (bw_win *win, unsigned long c, const bw_point *p, int n)
{
    x11_win *xw = win->impl;
    XPoint pts[16];
    int i;

    if (n > 16)
    {
        n = 16;
    }
    for (i = 0; i < n; i++)
    {
        pts[i].x = (short) p[i].x;
        pts[i].y = (short) p[i].y;
    }
    XSetForeground (d, xw->gc, c);
    XFillPolygon (d, target_of (win), xw->gc, pts, n, Convex, CoordModeOrigin);
}

static void x11_text (bw_win *win, unsigned long c, int x, int y, const char *s)
{
    x11_win *xw = win->impl;

    XSetForeground (d, xw->gc, c);
    XDrawString (d, target_of (win), xw->gc, x, y, s, (int) strlen (s));
}

static void x11_clip (bw_win *win, int x, int y, int width, int height)
{
    x11_win *xw = win->impl;

    if (width < 0)
    {
        XSetClipMask (d, xw->gc, None);
    }
    else
    {
        XRectangle clip;

        clip.x = (short) x;
        clip.y = (short) y;
        clip.width = (unsigned short) width;
        clip.height = (unsigned short) height;
        XSetClipRectangles (d, xw->gc, 0, 0, &clip, 1, Unsorted);
    }
}

static void x11_copy (bw_win *src, bw_win *dst, int sx, int sy,
                      int width, int height, int dx, int dy)
{
    XCopyArea (d, target_of (src), target_of (dst),
               ((x11_win *) dst->impl)->gc, sx, sy,
               (unsigned) width, (unsigned) height, dx, dy);
}

static void *x11_frame_pixels (bw_win *win, int *stride)
{
    x11_win *xw = win->impl;
    int major, minor;
    Bool pixmaps;

    if (xw->frame != NULL)
    {
        if (xw->frame->width == win->w && xw->frame->height == win->h)
        {
            *stride = xw->frame->bytes_per_line;

            return xw->frame->data;
        }
        frame_drop (xw);
        xw->frame_failed = 0;
    }
    if (xw->frame_failed)
    {
        return NULL;
    }
    xw->frame_failed = 1;
    if (!XShmQueryVersion (d, &major, &minor, &pixmaps))
    {
        return NULL;
    }
    xw->frame = XShmCreateImage (d, DefaultVisual (d, scr),
                                 (unsigned) DefaultDepth (d, scr), ZPixmap,
                                 NULL, &xw->shm, (unsigned) win->w,
                                 (unsigned) win->h);
    if (xw->frame == NULL)
    {
        return NULL;
    }
    /* The frame loops write 32-bit pixels; anything else would overrun */
    if (xw->frame->bits_per_pixel != 32)
    {
        XDestroyImage (xw->frame);
        xw->frame = NULL;

        return NULL;
    }
    xw->shm.shmid = shmget (IPC_PRIVATE,
                            xw->frame->bytes_per_line * xw->frame->height,
                            IPC_CREAT | 0600);
    if (xw->shm.shmid < 0)
    {
        XDestroyImage (xw->frame);
        xw->frame = NULL;

        return NULL;
    }
    xw->shm.shmaddr = shmat (xw->shm.shmid, NULL, 0);
    if (xw->shm.shmaddr == (void *) -1)
    {
        shmctl (xw->shm.shmid, IPC_RMID, NULL);
        XDestroyImage (xw->frame);
        xw->frame = NULL;

        return NULL;
    }
    xw->frame->data = xw->shm.shmaddr;
    xw->shm.readOnly = False;
    if (!XShmAttach (d, &xw->shm))
    {
        shmdt (xw->shm.shmaddr);
        shmctl (xw->shm.shmid, IPC_RMID, NULL);
        XDestroyImage (xw->frame);
        xw->frame = NULL;

        return NULL;
    }
    XSync (d, False);
    /*
     * Marked for removal now the server has it, since the sync above has
     * already carried the attach there: the kernel frees the segment when the
     * last attach goes away, so nothing this program can die of - benchmark.sh
     * kills the whole process group on Ctrl-C - leaves 8 MB behind for the
     * lifetime of the machine.
     */
    shmctl (xw->shm.shmid, IPC_RMID, NULL);
    xw->frame_failed = 0;
    *stride = xw->frame->bytes_per_line;

    return xw->frame->data;
}

static void x11_frame_size (bw_win *win, int *width, int *height)
{
    if (width != NULL) *width = win->w;
    if (height != NULL) *height = win->h;
}

static void x11_frame_push (bw_win *win)
{
    x11_win *xw = win->impl;

    if (xw->frame == NULL)
    {
        return;
    }
    /* The image's own size, not the window's: between a resize and the next
       x11_frame_pixels the two disagree, and only one of them is allocated */
    XShmPutImage (d, xw->xid, xw->gc, xw->frame, 0, 0, 0, 0,
                  (unsigned) xw->frame->width, (unsigned) xw->frame->height,
                  False);
}

static void x11_present (bw_win *win)
{
    (void) win;
    XFlush (d);
}

static void x11_sync (void)
{
    XSync (d, False);
}

static void x11_pump (void)
{
}

/* A failed grab must come back as no image, not as a killed process */
static int swallow_x_error (Display *dd, XErrorEvent *e)
{
    (void) dd;
    (void) e;

    return 0;
}

static bw_image *x11_capture (int x, int y, int w, int h)
{
    const char *cmd = getenv ("BENCH_CAPTURE_CMD");
    XImage *img;
    int (*old_handler) (Display *, XErrorEvent *);

    if (cmd != NULL && cmd[0] != '\0')
    {
        return capture_via_cmd (x, y, w, h, 1,
                                DisplayWidth (d, scr), DisplayHeight (d, scr));
    }
    XSync (d, False);
    old_handler = XSetErrorHandler (swallow_x_error);
    img = XGetImage (d, root, x, y, (unsigned) w, (unsigned) h,
                     AllPlanes, ZPixmap);
    XSync (d, False);
    XSetErrorHandler (old_handler);

    return capture_wrap_ximage (img);
}

static int x11_verify_at (bw_win *win)
{
    (void) win;

    /* The server already said where the window is; there is nothing to prove */
    return 1;
}

static void *x11_native_display (void)
{
    return NULL;
}

static void *x11_native_surface (bw_win *win)
{
    (void) win;

    return NULL;
}

/* restack keeps its own X11 tree walk; these are the Wayland way of asking */
static int x11_foreign_available (void)          { return 0; }
static int x11_foreign_exists (const char *t)    { (void) t; return 0; }
static int x11_foreign_activate (const char *t)  { (void) t; return 0; }

const struct bw_ops bw_x11_ops = {
    x11_open, x11_close, x11_screen_size, x11_stage,
    x11_create, x11_destroy, x11_map, x11_unmap, x11_wait_shown,
    x11_raise, x11_activate, x11_restore, x11_take_focus,
    x11_win_placed, x11_win_aimable, x11_move_raw, x11_where, x11_where_live, x11_resize,
    x11_maximize, x11_minimize, x11_fullscreen, x11_state,
    x11_opacity, x11_opaque_region, x11_background_colour, x11_set_background,
    x11_canvas_new,
    x11_fill, x11_rect, x11_poly, x11_text, x11_clip, x11_copy,
    x11_frame_pixels, x11_frame_size, x11_frame_push,
    x11_present, x11_sync, x11_pump,
    x11_capture, x11_verify_at,
    x11_native_display, x11_native_surface,
    x11_foreign_available, x11_foreign_exists, x11_foreign_activate,
};
