/*
 * Does the screen come back correctly after compositing suspends and resumes?
 *
 * The "suspend compositing for focused fullscreen windows" option is on by
 * default. When a fullscreen window takes focus the compositor detaches its
 * GLX drawable and unmaps the overlay; when that window goes away it reattaches
 * and has to repaint everything. Nothing here has ever checked that it does.
 * It is a user-visible path, on by default.
 *
 * A known band pattern sits in a window. A second window goes fullscreen,
 * which suspends compositing, then stops being fullscreen, which resumes it.
 * The pattern window must be exactly the pattern afterwards.
 *
 *   suspend_check [rounds]
 *
 * Exit status 0 when every resume came back clean.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"
#include "polite.h"
#include <X11/Xatom.h>

#define BAND    40
#define NCOL    8
#define WINW    900
#define WINH    700
#define MARGIN  20

static const unsigned long palette[NCOL] = {
    0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    0xff00ff, 0x00ffff, 0xffffff, 0x808080
};

static int band_of (int y)
{
    return (y / BAND) % NCOL;
}

static int colour_index (unsigned long pixel)
{
    int i;

    for (i = 0; i < NCOL; i++)
    {
        if ((pixel & 0xffffff) == palette[i])
        {
            return i;
        }
    }

    return -1;
}

/* Ask the window manager to put a window fullscreen, or take it out again */
static void set_fullscreen (Display *d, Window w, int on)
{
    XClientMessageEvent ev;

    memset (&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = XInternAtom (d, "_NET_WM_STATE", False);
    ev.format = 32;
    ev.data.l[0] = on ? 1 : 0;
    ev.data.l[1] = XInternAtom (d, "_NET_WM_STATE_FULLSCREEN", False);
    ev.data.l[3] = 1;
    XSendEvent (d, DefaultRootWindow (d), False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &ev);
    XFlush (d);
}

/* Every sampled pixel is the colour the pattern calls for */
static int capture_matches (Display *d, Window root, int ox, int oy)
{
    XImage *img;
    int x, y, ok = 1;

    img = capture_region (d, root, ox + MARGIN, oy + MARGIN,
                     WINW - 2 * MARGIN, WINH - 2 * MARGIN);
    if (img == NULL)
    {
        return -1;
    }
    for (y = 0; y < img->height && ok; y += 2)
    {
        for (x = 0; x < 5; x++)
        {
            int px = (img->width / 6) * (x + 1);

            if (colour_index (XGetPixel (img, px, y)) != band_of (y + MARGIN))
            {
                ok = 0;
                break;
            }
        }
    }
    XDestroyImage (img);

    return ok;
}

int main (int argc, char **argv)
{
    Display *d;
    Window root, win, fs, child;
    GC gc, gcfs;
    XSizeHints hints;
    Atom wtype, normal;
    int scr, rounds = (argc > 1) ? atoi (argv[1]) : 3;
    int r, i, y, ox, oy, ok = 0, bad = 0, inconclusive = 0;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);

    win = XCreateSimpleWindow (d, root, 140, 140, WINW, WINH, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = 140; hints.y = 140; hints.width = WINW; hints.height = WINH;
    XSetWMNormalHints (d, win, &hints);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, win, "suspend_check pattern");
    XMapWindow (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (3);

    XTranslateCoordinates (d, win, root, 0, 0, &ox, &oy, &child);

    for (r = 0; r < rounds; r++)
    {
        for (y = 0; y < WINH; y += BAND)
        {
            XSetForeground (d, gc, palette[band_of (y)]);
            XFillRectangle (d, win, gc, 0, y, WINW, BAND);
        }
        XSync (d, False);
        usleep (300000);

        /*
         * A managed window that asks the manager for fullscreen and never says
         * anything about bypassing the compositor: the category the option acts
         * on. Compositing should suspend while it has focus.
         */
        fs = XCreateSimpleWindow (d, root, 0, 0, 800, 600, 0,
                                  BlackPixel (d, scr), BlackPixel (d, scr));
        XChangeProperty (d, fs, wtype, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *) &normal, 1);
        XStoreName (d, fs, "suspend_check fullscreen");
        /*
         * The suspend only fires for the window that has the focus, so this
         * has to ask for it. Without the input hint the manager may never give
         * it, and then the whole check passes without having tested anything.
         */
        {
            XWMHints wm;

            wm.flags = InputHint | StateHint;
            wm.input = True;
            wm.initial_state = NormalState;
            XSetWMHints (d, fs, &wm);
        }
        XSelectInput (d, fs, StructureNotifyMask | FocusChangeMask);
        XMapRaised (d, fs);
        XSync (d, False);
        sleep (1);
        gcfs = XCreateGC (d, fs, 0, NULL);

        set_fullscreen (d, fs, 1);
        XSync (d, False);
        sleep (1);

        /* Ask the manager to activate it, then take the focus directly too */
        polite_activate (d, root, fs);
        XSetInputFocus (d, fs, RevertToPointerRoot, CurrentTime);
        XSync (d, False);
        sleep (2);
        /* Draw in it, so the suspended path has real work going through it */
        for (i = 0; i < 30; i++)
        {
            XSetForeground (d, gcfs, palette[i % NCOL]);
            XFillRectangle (d, fs, gcfs, (i * 97) % 1200, (i * 61) % 800,
                            500, 400);
            XSync (d, False);
            usleep (30000);
        }

        /* Out of fullscreen, then gone: compositing has to resume */
        set_fullscreen (d, fs, 0);
        XSync (d, False);
        sleep (1);
        XFreeGC (d, gcfs);
        XDestroyWindow (d, fs);
        XSync (d, False);
        sleep (2);

        /*
         * Look before redrawing. While compositing was suspended this window
         * was unredirected, so being covered destroyed its contents and the
         * server expects the client to paint them again. That is ordinary X11,
         * and it is also the proof that the suspend actually happened: with
         * compositing running the contents would have survived in the offscreen
         * pixmap. If this capture still matches, the suspend never fired and
         * the round has tested nothing, which is worth saying out loud.
         */
        if (capture_matches (d, root, ox, oy) == 1)
        {
            printf ("round %d: contents survived, so compositing never "
                    "suspended and this round proves nothing\n", r + 1);
            inconclusive++;
        }

        /* Now redraw: does what the client draws after the resume get through? */
        for (y = 0; y < WINH; y += BAND)
        {
            XSetForeground (d, gc, palette[band_of (y)]);
            XFillRectangle (d, win, gc, 0, y, WINW, BAND);
        }
        XSync (d, False);
        usleep (600000);

        switch (capture_matches (d, root, ox, oy))
        {
            case 1:
                ok++;
                break;
            case -1:
                printf ("round %d: the capture failed, so this round proves "
                        "nothing\n", r + 1);
                inconclusive++;
                break;
            default:
                bad++;
                printf ("round %d: the screen did not come back\n", r + 1);
        }
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("suspend and resume: %d clean, %d wrong, %d proved nothing\n",
            ok, bad, inconclusive);

    return (bad == 0 && inconclusive == 0 && ok > 0) ? 0 : 1;
}
