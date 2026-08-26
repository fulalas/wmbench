/*
 * Shaped windows, which window_shape() exists for and nothing has ever checked.
 *
 * A window can declare that it is not a rectangle. The compositor has to draw
 * only the declared part and let whatever is behind show through the rest; get
 * it wrong and a shaped window paints its undefined corners over the desktop.
 * It matters more than it sounds: themes with rounded corners shape the frame,
 * so this is not only an oddity of old applications.
 *
 * A background window is filled with one colour. A shaped window on top of it
 * is filled with another, and shaped to a band down its middle. Inside the band
 * the top colour must show; outside it, within the same bounding box, the
 * background colour must show through.
 *
 *   shape_check [rounds]
 *
 * Exit status 0 when the shape was honoured both ways.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include "win.h"

#define BGW     1000
#define BGH     760
#define FGW     600
#define FGH     460
#define BANDX   200          /* the shaped band, in window coordinates */
#define BANDW   200

#define BG_COLOUR   0x00ff00
#define FG_COLOUR   0xff0000

static Window make_window (Display *d, int scr, int x, int y, int w, int h,
                           const char *name)
{
    Window win;
    XSizeHints hints;
    Atom wtype, normal;

    win = XCreateSimpleWindow (d, RootWindow (d, scr), x, y, w, h, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = x; hints.y = y; hints.width = w; hints.height = h;
    XSetWMNormalHints (d, win, &hints);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, win, name);

    return win;
}

int main (int argc, char **argv)
{
    Display *d;
    Window root, bg, fg, child;
    GC gc;
    bw_image *img;
    XRectangle band;
    XWindowAttributes bga, fga;
    int scr, ev, err, rounds = (argc > 1) ? atoi (argv[1]) : 2;
    int r, y, bx, by, fx, fy, ok = 0, bad = 0;

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    if (bw_is_wayland ())
    {
        printf ("wayland has no shape concept; the defect lives in "
                "per-pixel alpha there, which argbbench exercises\n");

        return 3;
    }
    /* The shape extension is X11's own; asked of X11 directly */
    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    if (!XShapeQueryExtension (d, &ev, &err))
    {
        fprintf (stderr, "no XShape\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    bg = make_window (d, scr, 120, 120, BGW, BGH, "shape_check background");
    fg = make_window (d, scr, 240, 240, FGW, FGH, "shape_check shaped");

    /* Only a band down the middle of the shaped window exists at all */
    band.x = BANDX;
    band.y = 0;
    band.width = BANDW;
    band.height = FGH;
    XShapeCombineRectangles (d, fg, ShapeBounding, 0, 0, &band, 1,
                             ShapeSet, Unsorted);

    XMapWindow (d, bg);
    XMapRaised (d, fg);
    gc = XCreateGC (d, bg, 0, NULL);
    XSync (d, False);
    sleep (3);

    XTranslateCoordinates (d, bg, root, 0, 0, &bx, &by, &child);
    XTranslateCoordinates (d, fg, root, 0, 0, &fx, &fy, &child);

    /*
     * The size was asked for with USSize, which is a hint and not a promise: a
     * manager that gives either window a size of its own choosing is within
     * its rights. Every sample point below is a fixed one in the size that was
     * asked for, so on a different size they land outside the window and read
     * the wrong colour, which would be reported as a shape the compositor drew
     * wrong. Say what happened instead of blaming it for that.
     */
    if (!XGetWindowAttributes (d, bg, &bga) ||
        !XGetWindowAttributes (d, fg, &fga) ||
        bga.width != BGW || bga.height != BGH ||
        fga.width != FGW || fga.height != FGH)
    {
        printf ("the windows are not the size they asked for, "
                "so this would prove nothing\n");

        return 2;
    }

    if (fx < bx || fy < by ||
        fx + FGW > bx + BGW || fy + FGH > by + BGH)
    {
        printf ("the shaped window is not inside the background one, "
                "so this would prove nothing\n");

        return 2;
    }

    for (r = 0; r < rounds; r++)
    {
        int wrong_in = 0, wrong_out = 0, checked = 0;

        XSetForeground (d, gc, BG_COLOUR);
        XFillRectangle (d, bg, gc, 0, 0, BGW, BGH);
        XSetForeground (d, gc, FG_COLOUR);
        XFillRectangle (d, fg, gc, 0, 0, FGW, FGH);
        XSync (d, False);
        usleep (700000);

        img = bw_capture (fx, fy, FGW, FGH);
        if (img == NULL)
        {
            fprintf (stderr, "capture failed\n");

            return 2;
        }

        for (y = 20; y < FGH - 20; y += 4)
        {
            unsigned long inside, outside;

            /* Inside the band: the shaped window's own colour */
            inside = bw_pixel (img, BANDX + BANDW / 2, y);
            /* Left of the band, still inside the window box: the background */
            outside = bw_pixel (img, BANDX / 2, y);
            checked++;

            if (inside != FG_COLOUR)
            {
                wrong_in++;
            }
            if (outside != BG_COLOUR)
            {
                wrong_out++;
            }
        }
        bw_image_free (img);

        if (wrong_in == 0 && wrong_out == 0)
        {
            ok++;
        }
        else
        {
            bad++;
            printf ("round %d: of %d rows, %d wrong inside the shape and "
                    "%d wrong outside it\n", r + 1, checked, wrong_in,
                    wrong_out);
        }
    }

    XDestroyWindow (d, fg);
    XDestroyWindow (d, bg);
    XCloseDisplay (d);
    bw_close ();

    printf ("shaped windows: %d clean, %d wrong\n", ok, bad);

    return (bad == 0 && ok > 0) ? 0 : 1;
}
