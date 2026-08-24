/*
 * The screen before a load against the same screen after it.
 *
 *   leftover_check save <file>    photograph the idle screen now
 *   leftover_check check <file>   photograph it again and compare
 *
 * An idle screen that nobody touched has to come back to the pixels it had.
 * What differs is what the compositor drew and never took back: garbage left
 * where a window used to be, a region it never repainted.
 *
 * Only the desktop around the windows is judged. Every window that is on
 * screen is left out of the comparison, because a clock in a panel or a
 * terminal with output in it changes on its own and says nothing about the
 * compositor. What the load's own windows leave behind is on the desktop by
 * then, which is exactly what is looked at.
 *
 * save photographs twice, a second apart, and refuses the job if even the
 * desktop is changing on its own.
 *
 * Exit status 0 when the screen came back, 1 when too much of it did not,
 * 2 when it cannot be photographed, 3 when there was nothing to compare.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include "capture.h"

/* Summed over the three channels, so a hair of dithering is not a difference */
#define TOL      24
#define IDLE_PC  0.20           /* the desktop changing this much: not idle */
#define BAD_PC   0.50           /* differing this much afterwards: leftovers */
#define MARGIN   48             /* shadows and frames sit outside the window */
#define MIN_OPEN 2.0            /* less desktop than this on view: no answer */

/* Is this the window the desktop itself is drawn on? Under a window manager
   that reparents, the answer is on the client window one level down. */
static int is_desktop (Display *d, Window w, int depth)
{
    Atom type = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    Atom wanted = XInternAtom (d, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    Atom got = None;
    int fmt;
    unsigned long n = 0, rest;
    unsigned char *p = NULL;
    Window rr, parent, *kids = NULL;
    unsigned int nk = 0, i;
    int found = 0;

    if (XGetWindowProperty (d, w, type, 0, 1, False, XA_ATOM, &got, &fmt,
                            &n, &rest, &p) == Success && p != NULL)
    {
        found = (n > 0 && *(Atom *) p == wanted);
        XFree (p);

        return found;
    }
    if (depth <= 0)
    {
        return 0;
    }
    if (XQueryTree (d, w, &rr, &parent, &kids, &nk))
    {
        for (i = 0; i < nk && !found; i++)
        {
            found = is_desktop (d, kids[i], depth - 1);
        }
        if (kids != NULL)
        {
            XFree (kids);
        }
    }

    return found;
}

/* One byte a pixel: 1 where a window is, and so where nothing is judged */
static unsigned char *covered_mask (Display *d, Window root, int w, int h)
{
    unsigned char *m;
    Window rr, parent, *kids = NULL;
    unsigned int n = 0, i;

    m = calloc ((size_t) w * h, 1);
    if (m == NULL)
    {
        return NULL;
    }
    if (!XQueryTree (d, root, &rr, &parent, &kids, &n))
    {
        return m;
    }
    for (i = 0; i < n; i++)
    {
        XWindowAttributes a;
        int x0, x1, y0, y1, row;

        if (!XGetWindowAttributes (d, kids[i], &a))
        {
            continue;
        }
        if (a.map_state != IsViewable || a.class != InputOutput)
        {
            continue;
        }
        if (is_desktop (d, kids[i], 2))
        {
            continue;
        }
        /* A child of the root is already in root coordinates */
        x0 = a.x - MARGIN < 0 ? 0 : a.x - MARGIN;
        y0 = a.y - MARGIN < 0 ? 0 : a.y - MARGIN;
        x1 = a.x + a.width + MARGIN > w ? w : a.x + a.width + MARGIN;
        y1 = a.y + a.height + MARGIN > h ? h : a.y + a.height + MARGIN;
        if (x1 > x0)
        {
            for (row = y0; row < y1; row++)
            {
                memset (m + (size_t) row * w + x0, 1, (size_t) (x1 - x0));
            }
        }
    }
    if (kids != NULL)
    {
        XFree (kids);
    }

    return m;
}

/*
 * How much of the desktop on file the image in hand does not match, as a
 * percentage of the pixels that are desktop in both, with the first and last
 * row that differ. Negative when the file cannot be read or is of another
 * screen; *open is how much of the screen was judged at all.
 */
static double compare_ppm (const char *path, XImage *img,
                           const unsigned char *mask, double *open,
                           int *y0, int *y1)
{
    FILE *f;
    unsigned char *row;
    int w = 0, h = 0, mx = 0, x, y;
    long bad = 0, seen = 0;

    *y0 = -1;
    *y1 = -1;
    *open = 0;
    f = fopen (path, "rb");
    if (f == NULL)
    {
        return -1;
    }
    if (fscanf (f, "P6 %d %d %d", &w, &h, &mx) != 3 || mx != 255 ||
        w != img->width || h != img->height || fgetc (f) == EOF)
    {
        fclose (f);

        return -1;
    }
    row = malloc ((size_t) w * 3);
    if (row == NULL)
    {
        fclose (f);

        return -1;
    }
    for (y = 0; y < h; y++)
    {
        if (fread (row, 3, (size_t) w, f) != (size_t) w)
        {
            free (row);
            fclose (f);

            return -1;
        }
        for (x = 0; x < w; x++)
        {
            unsigned long p;
            int dr, dg, db;

            if (mask[(size_t) y * w + x])
            {
                continue;
            }
            seen++;
            p = XGetPixel (img, x, y);
            dr = (int) ((p >> 16) & 0xff) - row[x * 3];
            dg = (int) ((p >> 8) & 0xff) - row[x * 3 + 1];
            db = (int) (p & 0xff) - row[x * 3 + 2];
            if (abs (dr) + abs (dg) + abs (db) > TOL)
            {
                bad++;
                if (*y0 < 0)
                {
                    *y0 = y;
                }
                *y1 = y;
            }
        }
    }
    free (row);
    fclose (f);
    *open = 100.0 * (double) seen / ((double) w * h);

    return seen ? 100.0 * (double) bad / (double) seen : -1;
}

int main (int argc, char **argv)
{
    Display *d;
    Window root;
    XWindowAttributes wa;
    XImage *img;
    unsigned char *mask;
    double pc, open;
    int y0, y1;

    if (argc < 3 || (strcmp (argv[1], "save") != 0 &&
                     strcmp (argv[1], "check") != 0))
    {
        fprintf (stderr, "usage: leftover_check save|check <file>\n");

        return 2;
    }
    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    root = RootWindow (d, DefaultScreen (d));
    XGetWindowAttributes (d, root, &wa);

    img = capture_region (d, root, 0, 0, wa.width, wa.height);
    if (img == NULL)
    {
        fprintf (stderr, "the screen cannot be photographed\n");
        XCloseDisplay (d);

        return 2;
    }
    /* The windows as they are now: what is under them is nobody's business */
    mask = covered_mask (d, root, wa.width, wa.height);
    if (mask == NULL)
    {
        XDestroyImage (img);
        XCloseDisplay (d);

        return 2;
    }

    if (strcmp (argv[1], "save") == 0)
    {
        if (!capture_write_ppm (argv[2], img))
        {
            fprintf (stderr, "cannot write %s\n", argv[2]);

            return 2;
        }
        XDestroyImage (img);

        /* Is this screen still at all? */
        sleep (1);
        img = capture_region (d, root, 0, 0, wa.width, wa.height);
        if (img == NULL)
        {
            fprintf (stderr, "the screen cannot be photographed\n");

            return 2;
        }
        pc = compare_ppm (argv[2], img, mask, &open, &y0, &y1);
        if (pc < 0 || open < MIN_OPEN)
        {
            printf ("the desktop is covered (%.0f%% of the screen is left), "
                    "so leftovers on it cannot be seen\n", open);

            return 3;
        }
        if (pc > IDLE_PC)
        {
            printf ("the desktop was busy on its own (%.2f%% of it changed "
                    "between two photographs a second apart)\n", pc);

            return 3;
        }

        return 0;
    }

    pc = compare_ppm (argv[2], img, mask, &open, &y0, &y1);
    if (pc < 0)
    {
        fprintf (stderr, "no usable photograph of the screen before the load\n");

        return 2;
    }
    if (open < MIN_OPEN)
    {
        printf ("the desktop is covered (%.0f%% of the screen is left), "
                "so leftovers on it cannot be seen\n", open);

        return 3;
    }
    if (pc > BAD_PC)
    {
        printf ("%.2f%% of the desktop did not come back after the load, "
                "rows %d to %d\n", pc, y0, y1);

        return 1;
    }
    printf ("the desktop came back (%.2f%% differs, %.0f%% of the screen "
            "judged)\n", pc, open);

    return 0;
}
