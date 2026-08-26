/*
 * Put windows in a known order, bottom first:
 *
 *   restack "fsbench" "transbench background" "movebench"
 *
 * The stress mix starts six programs at once, and the order they end up
 * stacked in is whatever the window manager happened to do with them. That
 * decides which windows are covered, and a covered window is not composited -
 * so the same test could measure noticeably different amounts of work from
 * one run to the next. Naming the order makes every run identical.
 *
 * A name may match several windows (the many-window filler); all of them are
 * raised, in the order the server lists them. The window with the name is the
 * client, and it is the client that is named in the request: see ask_raise()
 * for why raising the frame the window manager put around it does nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include "win.h"

static Display *d;
static Window root;

/*
 * The Wayland answer. Only a compositor that lists foreign toplevels can be
 * asked about other programs' windows at all, and even that list carries no
 * stacking order, so -c can never verify one here; the stress mix on Wayland
 * relies on open order and the WINDOW-UP marker instead of this program.
 */
static int wayland_main (int argc, char **argv)
{
    int i, first = 1, wait_only = 0, check_only = 0, never = 0, total = 0;

    if (argc > 1 && strcmp (argv[1], "-w") == 0)
    {
        first = 2;
    }
    if (argc > 1 && strcmp (argv[1], "-wait") == 0)
    {
        wait_only = 1;
        first = 2;
    }
    if (argc > 1 && strcmp (argv[1], "-c") == 0)
    {
        check_only = 1;
        first = 2;
    }
    if (!bw_foreign_available ())
    {
        printf ("restack: this session does not list other windows\n");
        fflush (stdout);

        return 3;
    }
    if (first == 2 && !check_only)
    {
        for (i = first; i < argc; i++)
        {
            int waited;

            for (waited = 0; waited < 300; waited++)   /* up to 30 seconds */
            {
                if (bw_foreign_exists (argv[i]))
                {
                    break;
                }
                usleep (100000);
            }
            if (waited >= 300)
            {
                printf ("restack: waited in vain for \"%s\"\n", argv[i]);
                never = 1;
            }
        }
    }
    if (wait_only)
    {
        return never ? 3 : 0;
    }
    if (!check_only)
    {
        for (i = first; i < argc; i++)
        {
            int n = bw_foreign_activate (argv[i]);

            if (n == 0)
            {
                printf ("restack: nothing named \"%s\"\n", argv[i]);
            }
            total += n;
            usleep (40000);
        }
        printf ("restack: %d windows placed\n", total);
    }
    printf ("stack: cannot be verified on this session\n");
    fflush (stdout);

    return 3;
}

/*
 * The tree is walked on a live desktop: a menu, a tooltip or a benchmark
 * window whose load has just finished can be gone between the XQueryTree
 * reply and the XFetchName that follows it. Xlib's default handler would end
 * the process there, and a caller reading no "stack:" line at all blames the
 * window manager for a race in here.
 */
static int swallow_x_error (Display *dd, XErrorEvent *e)
{
    (void) dd;
    (void) e;

    return 0;
}

/*
 * Ask for a window to be raised. Raising the frame the window manager put
 * around it does nothing: the frame belongs to the window manager, and it
 * ignores another program restacking it. The request has to name the client
 * window - as a pager does, through _NET_RESTACK_WINDOW - and the plain call
 * on the client is sent as well, for anything that does not know that
 * message.
 */
static void ask_raise (Window client)
{
    XClientMessageEvent ev;

    memset (&ev, 0, sizeof ev);
    ev.type = ClientMessage;
    ev.window = client;
    ev.message_type = XInternAtom (d, "_NET_RESTACK_WINDOW", False);
    ev.format = 32;
    ev.data.l[0] = 2;           /* a pager asks */
    ev.data.l[1] = None;        /* no sibling: the top of the stack */
    ev.data.l[2] = Above;
    XSendEvent (d, root, False,
                SubstructureNotifyMask | SubstructureRedirectMask,
                (XEvent *) &ev);
    XRaiseWindow (d, client);
}

/* Every window under w whose name matches, raised in the order found */
static int raise_matching (Window w, const char *name, int depth)
{
    Window r, parent, *kids = NULL;
    unsigned int n, i;
    char *wname = NULL;
    int raised = 0;

    if (depth > 4)
    {
        return 0;
    }
    if (XFetchName (d, w, &wname) && wname != NULL)
    {
        if (strcmp (wname, name) == 0)
        {
            ask_raise (w);
            raised = 1;
        }
        XFree (wname);
    }
    if (raised)
    {
        return 1;
    }
    if (XQueryTree (d, w, &r, &parent, &kids, &n))
    {
        for (i = 0; i < n; i++)
        {
            raised += raise_matching (kids[i], name, depth + 1);
        }
        if (kids != NULL)
        {
            XFree (kids);
        }
    }

    return raised;
}

/* Is there a viewable window of this name yet? */
static int have_window (Window w, const char *name, int depth)
{
    Window r, parent, *kids = NULL;
    unsigned int n, i;
    char *wname = NULL;
    int found = 0;
    XWindowAttributes a;

    if (depth > 4)
    {
        return 0;
    }
    if (XFetchName (d, w, &wname) && wname != NULL)
    {
        if (strcmp (wname, name) == 0 && XGetWindowAttributes (d, w, &a) &&
            a.map_state == IsViewable)
        {
            found = 1;
        }
        XFree (wname);
    }
    if (found)
    {
        return 1;
    }
    if (XQueryTree (d, w, &r, &parent, &kids, &n))
    {
        for (i = 0; i < n && !found; i++)
        {
            found = have_window (kids[i], name, depth + 1);
        }
        if (kids != NULL)
        {
            XFree (kids);
        }
    }

    return found;
}

/* Does this window, or one under it, carry this name? */
static int matches (Window w, const char *name, int depth)
{
    Window r, parent, *kids = NULL;
    unsigned int n, i;
    char *wname = NULL;
    int found = 0;

    if (depth > 4)
    {
        return 0;
    }
    if (XFetchName (d, w, &wname) && wname != NULL)
    {
        found = (strcmp (wname, name) == 0);
        XFree (wname);
    }
    if (found)
    {
        return 1;
    }
    if (XQueryTree (d, w, &r, &parent, &kids, &n))
    {
        for (i = 0; i < n && !found; i++)
        {
            found = matches (kids[i], name, depth + 1);
        }
        if (kids != NULL)
        {
            XFree (kids);
        }
    }

    return found;
}

/*
 * Is the stack the order that was asked for? Only the named windows are
 * looked at, from the bottom up; anything else on the desktop sits wherever
 * it likes. A name may appear more than once - the many-window filler - so a
 * repeat of the name just seen is not out of order.
 */
static int stack_wrong (int argc, char **argv, int first, int say)
{
    Window r, parent, *kids = NULL;
    unsigned int n, k;
    int want = first, last = -1, bad = 0, i;

    XSync (d, False);
    if (!XQueryTree (d, root, &r, &parent, &kids, &n))
    {
        return 0;
    }
    for (k = 0; k < n; k++)
    {
        Window kid = kids[k];   /* XQueryTree lists the bottom-most first */

        for (i = first; i < argc; i++)
        {
            if (matches (kid, argv[i], 0))
            {
                break;
            }
        }
        if (i == argc || i == last)
        {
            continue;           /* not ours, or another of the same name */
        }
        if (i != want)
        {
            bad = 1;
            if (say)
            {
                printf ("stack: expected \"%s\" here, found \"%s\"\n",
                        (want < argc) ? argv[want] : "nothing", argv[i]);
            }
        }
        last = i;
        want = i + 1;
    }
    if (kids != NULL)
    {
        XFree (kids);
    }
    if (want < argc)
    {
        bad = 1;
        if (say)
        {
            printf ("stack: never saw \"%s\"\n", argv[want]);
        }
    }

    return bad;
}

int main (int argc, char **argv)
{
    int i, total = 0, first = 1, wait_first = 0, wait_only = 0,
        check_only = 0, never = 0;
    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    if (bw_is_wayland ())
    {
        return wayland_main (argc, argv);
    }
    bw_close ();

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    root = DefaultRootWindow (d);
    XSetErrorHandler (swallow_x_error);

    /*
     * -w: wait for every one of these windows to be on screen before
     * touching the stack. A test must not start while its own windows are
     * still appearing: one that arrives late lands on top of the order that
     * was just arranged, and the first seconds measure a load that is not
     * all there yet.
     */
    if (argc > 1 && strcmp (argv[1], "-w") == 0)
    {
        wait_first = 1;
        first = 2;
    }
    /*
     * -wait: wait for these windows and stop there, raising nothing. The
     * stress mix opens its windows one at a time, each one waited for before
     * the next is started, so they stack in the order they were opened - the
     * way a person's desktop stacks. Nothing then has to be restacked, and
     * nothing depends on a window manager granting a raise.
     */
    if (argc > 1 && strcmp (argv[1], "-wait") == 0)
    {
        wait_first = 1;
        wait_only = 1;
        first = 2;
    }
    /* -c: say whether the order is the one named, and change nothing */
    if (argc > 1 && strcmp (argv[1], "-c") == 0)
    {
        check_only = 1;
        first = 2;
    }

    if (wait_first)
    {
        for (i = first; i < argc; i++)
        {
            int waited;

            for (waited = 0; waited < 300; waited++)   /* up to 30 seconds */
            {
                if (have_window (root, argv[i], 0))
                {
                    break;
                }
                usleep (100000);
            }
            if (waited >= 300)
            {
                printf ("restack: waited in vain for \"%s\"\n", argv[i]);
                never = 1;
            }
        }
    }

    if (wait_only)
    {
        XCloseDisplay (d);
        /*
         * A window that never came is not a stacking question - it is a load
         * that did not start - and saying so here is the difference between a
         * mix with a hole in it and a mix nobody knew had one.
         */
        return never ? 3 : 0;
    }

    /*
     * Twice if need be. The raise is a request, and a window manager with six
     * programs putting windows up at once answers it when it gets to it: a
     * stack read straight after the last request is often still settling, and
     * calling that a refusal would throw away a row that was perfectly good.
     */
    if (!check_only)
    {
        int pass;

        for (pass = 0; pass < 2; pass++)
        {
            total = 0;
            for (i = first; i < argc; i++)
            {
                int n = raise_matching (root, argv[i], 0);

                if (n == 0 && pass == 0)
                {
                    printf ("restack: nothing named \"%s\"\n", argv[i]);
                }
                total += n;
                XSync (d, False);
                usleep (40000);
            }
            usleep (500000);    /* let it settle before believing it */
            if (!stack_wrong (argc, argv, first, 0))
            {
                break;
            }
        }
    }
    if (!check_only)
    {
        printf ("restack: %d windows placed\n", total);
    }

    {
        int bad = stack_wrong (argc, argv, first, 1);

        printf ("stack: %s\n", bad ? "NOT AS ASKED" : "as asked");
        fflush (stdout);
        XCloseDisplay (d);

        return bad ? 3 : 0;
    }
}
