#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "polite.h"

/* The server's current time, fished out of a PropertyNotify on the window */
static Time server_time (Display *d, Window w)
{
    XWindowAttributes at;
    XEvent ev;
    Atom a = XInternAtom (d, "_BENCH_TIME", False);
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

void polite_activate (Display *d, Window root, Window w)
{
    XClientMessageEvent ev;

    memset (&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = w;
    ev.message_type = XInternAtom (d, "_NET_ACTIVE_WINDOW", False);
    ev.format = 32;
    ev.data.l[0] = 2;                   /* a pager asks */
    ev.data.l[1] = (long) server_time (d, w);
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
void polite_keep_above (Display *d, Window w)
{
    Atom above = XInternAtom (d, "_NET_WM_STATE_ABOVE", False);

    XChangeProperty (d, w, XInternAtom (d, "_NET_WM_STATE", False),
                     XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &above, 1);
}
