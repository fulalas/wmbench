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
 * client, but what has to be raised is the frame the window manager put
 * around it, so the search walks back up to the child of the root.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>

static Display *d;
static Window root;

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

int main (int argc, char **argv)
{
    int i, total = 0, first = 1, wait_first = 0;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    root = DefaultRootWindow (d);

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
            }
        }
    }

    for (i = first; i < argc; i++)
    {
        int n = raise_matching (root, argv[i], 0);

        if (n == 0)
        {
            printf ("restack: nothing named \"%s\"\n", argv[i]);
        }
        total += n;
        XSync (d, False);
        usleep (40000);
    }
    printf ("restack: %d windows placed\n", total);
    XCloseDisplay (d);

    return 0;
}
