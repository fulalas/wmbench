/*
 * The magnifier, which no test has ever touched.
 *
 * Alt and the scroll wheel over a window magnify the desktop. It is a real
 * feature, on by default (zoom_desktop), and the GL renderer draws it by
 * rendering the scene into a frame buffer object and then drawing that
 * magnified. Changing the presentation default from the scene buffer to the
 * swap moved that frame buffer off the path every frame takes and onto one that
 * only runs while magnifying, so it is now colder than it was and it was never
 * checked at all.
 *
 * A known band pattern sits in a window. Alt-scroll magnifies. The bands must
 * come out taller than they were, which is what proves the magnifier actually
 * engaged rather than the test passing for nothing. Then it zooms back out, and
 * the pattern must be exactly itself again.
 *
 *   zoom_check [rounds]
 *
 * Needs XTest to drive the pointer. Exit status 0 when every round magnified
 * and came back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#define BAND    40
#define NCOL    8
#define WINW    900
#define WINH    700
#define MARGIN  20

static const unsigned long palette[NCOL] = {
    0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    0xff00ff, 0x00ffff, 0xffffff, 0x808080
};

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

/*
 * The height in screen rows of the band the given column starts in. Unmagnified
 * that is BAND; magnified it is BAND times the zoom, which is how the check
 * knows the magnifier engaged.
 */
static int first_band_height (Display *d, Window root, int x, int y0, int y1)
{
    XImage *img;
    int y, first, run = 0;

    img = capture_region (d, root, x, y0, 1, y1 - y0);
    if (img == NULL)
    {
        return -1;
    }
    first = colour_index (XGetPixel (img, 0, 0));
    if (first < 0)
    {
        XDestroyImage (img);

        return -1;
    }
    for (y = 0; y < img->height; y++)
    {
        if (colour_index (XGetPixel (img, 0, y)) != first)
        {
            break;
        }
        run++;
    }
    XDestroyImage (img);

    return run;
}

static void scroll (Display *d, KeyCode alt, int button, int times)
{
    int i;

    XTestFakeKeyEvent (d, alt, True, 0);
    XSync (d, False);
    for (i = 0; i < times; i++)
    {
        XTestFakeButtonEvent (d, button, True, 0);
        XTestFakeButtonEvent (d, button, False, 0);
        XSync (d, False);
        usleep (150000);
    }
    XTestFakeKeyEvent (d, alt, False, 0);
    XSync (d, False);
    usleep (300000);
}

int main (int argc, char **argv)
{
    Display *d;
    Window root, win, child;
    GC gc;
    XSizeHints hints;
    Atom wtype, normal;
    KeyCode alt;
    int scr, ev, err, major, minor;
    int rounds = (argc > 1) ? atoi (argv[1]) : 2;
    int r, y, ox, oy, plain, zoomed_h, back;
    int ok = 0, bad = 0, inconclusive = 0;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    if (!XTestQueryExtension (d, &ev, &err, &major, &minor))
    {
        fprintf (stderr, "no XTest, cannot drive the pointer\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);
    alt = XKeysymToKeycode (d, XK_Alt_L);

    win = XCreateSimpleWindow (d, root, 160, 160, WINW, WINH, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = 160; hints.y = 160; hints.width = WINW; hints.height = WINH;
    XSetWMNormalHints (d, win, &hints);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, win, "zoom_check");
    XMapRaised (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (3);

    XTranslateCoordinates (d, win, root, 0, 0, &ox, &oy, &child);

    for (r = 0; r < rounds; r++)
    {
        for (y = 0; y < WINH; y += BAND)
        {
            XSetForeground (d, gc, palette[(y / BAND) % NCOL]);
            XFillRectangle (d, win, gc, 0, y, WINW, BAND);
        }
        XSync (d, False);
        usleep (400000);

        /* The pointer has to be over the window: the zoom rides a button press
         * delivered to a managed client. */
        XTestFakeMotionEvent (d, scr, ox + WINW / 2, oy + WINH / 2, 0);
        XSync (d, False);
        usleep (200000);

        plain = first_band_height (d, root, ox + WINW / 2,
                                   oy + MARGIN, oy + WINH - MARGIN);

        scroll (d, alt, Button4, 3);
        zoomed_h = first_band_height (d, root, ox + WINW / 2,
                                      oy + MARGIN, oy + WINH - MARGIN);

        /* Back out again, generously, then let it settle */
        scroll (d, alt, Button5, 8);
        usleep (600000);
        back = first_band_height (d, root, ox + WINW / 2,
                                  oy + MARGIN, oy + WINH - MARGIN);

        if (plain < 0 || zoomed_h < 0 || back < 0)
        {
            printf ("round %d: a capture failed, so this round proves "
                    "nothing\n", r + 1);
            inconclusive++;
        }
        else if (zoomed_h <= plain)
        {
            printf ("round %d: bands %d rows before and %d after, so the "
                    "magnifier never engaged and this round proves nothing\n",
                    r + 1, plain, zoomed_h);
            inconclusive++;
        }
        else if (back != plain)
        {
            printf ("round %d: magnified to %d rows but came back as %d, "
                    "not the %d it started at\n",
                    r + 1, zoomed_h, back, plain);
            bad++;
        }
        else
        {
            ok++;
        }
    }

    XDestroyWindow (d, win);
    XCloseDisplay (d);

    printf ("magnifier: %d clean, %d wrong, %d proved nothing\n",
            ok, bad, inconclusive);

    return (bad == 0 && inconclusive == 0 && ok > 0) ? 0 : 1;
}
