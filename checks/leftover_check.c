/*
 *   leftover_check save <file>    photograph the idle screen now
 *   leftover_check check <file>   photograph it again and compare
 *
 * An idle screen nobody touched has to come back to the pixels it had; what
 * differs is what the compositor drew and never took back.
 *
 * Only the desktop around the windows is judged. Every window on screen is left
 * out, because a clock in a panel or a terminal with output in it changes on
 * its own and says nothing about the compositor. What the load's own windows
 * left behind is on the desktop by then, which is what is looked at.
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
#include "win.h"

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
    /* An unreadable tree is a failure to look, not an uncovered desktop */
    if (!XQueryTree (d, root, &rr, &parent, &kids, &n))
    {
        free (m);

        return NULL;
    }
    for (i = 0; i < n; i++)
    {
        XWindowAttributes a;
        int x0, x1, y0, y1, line;

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
            for (line = y0; line < y1; line++)
            {
                memset (m + (size_t) line * w + x0, 1, (size_t) (x1 - x0));
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
 * line that differ. -1 when the file cannot be read or is of another screen,
 * -2 when the file was fine and the windows left no desktop pixel to judge;
 * the two are different answers and the caller has to tell them apart.
 * *open is how much of the screen was judged at all.
 */
static double compare_ppm (const char *path, bw_image *img,
                           const unsigned char *mask, double *open,
                           int *y0, int *y1)
{
    FILE *f;
    unsigned char *line;
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
    line = malloc ((size_t) w * 3);
    if (line == NULL)
    {
        fclose (f);

        return -1;
    }
    for (y = 0; y < h; y++)
    {
        if (fread (line, 3, (size_t) w, f) != (size_t) w)
        {
            free (line);
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
            p = bw_pixel (img, x, y);
            dr = (int) ((p >> 16) & 0xff) - line[x * 3];
            dg = (int) ((p >> 8) & 0xff) - line[x * 3 + 1];
            db = (int) (p & 0xff) - line[x * 3 + 2];
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
    free (line);
    fclose (f);
    *open = 100.0 * (double) seen / ((double) w * h);

    return seen ? 100.0 * (double) bad / (double) seen : -2;
}

int main (int argc, char **argv)
{
    Display *d;
    Window root;
    XWindowAttributes wa;
    bw_image *img = NULL;
    unsigned char *mask = NULL;
    double pc, open;
    int y0, y1, rc;

    if (argc < 3 || (strcmp (argv[1], "save") != 0 &&
                     strcmp (argv[1], "check") != 0))
    {
        fprintf (stderr, "usage: leftover_check save|check <file>\n");

        return 2;
    }
    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    if (bw_is_wayland ())
    {
        printf ("this session does not tell one window's place from "
                "another's, so nothing can be masked out\n");

        return 3;
    }
    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    root = RootWindow (d, DefaultScreen (d));
    XGetWindowAttributes (d, root, &wa);

    img = bw_capture (0, 0, wa.width, wa.height);
    if (img == NULL)
    {
        fprintf (stderr, "the screen cannot be photographed\n");
        rc = 2;
        goto out;
    }
    /* The windows as they are now: what is under them is nobody's business */
    mask = covered_mask (d, root, wa.width, wa.height);
    if (mask == NULL)
    {
        rc = 2;
        goto out;
    }

    if (strcmp (argv[1], "save") == 0)
    {
        if (!capture_write_ppm (argv[2], img))
        {
            fprintf (stderr, "cannot write %s\n", argv[2]);
            rc = 2;
            goto out;
        }
        bw_image_free (img);

        sleep (1);
        img = bw_capture (0, 0, wa.width, wa.height);
        if (img == NULL)
        {
            fprintf (stderr, "the screen cannot be photographed\n");
            rc = 2;
            goto out;
        }
        pc = compare_ppm (argv[2], img, mask, &open, &y0, &y1);
        if (pc == -2 || open < MIN_OPEN)
        {
            printf ("the desktop is covered (%.0f%% of the screen is left), "
                    "so leftovers on it cannot be seen\n", open);
            rc = 3;
            goto out;
        }
        if (pc < 0)
        {
            fprintf (stderr, "the photograph just saved cannot be read back\n");
            rc = 2;
            goto out;
        }
        if (pc > IDLE_PC)
        {
            printf ("the desktop was busy on its own (%.2f%% of it changed "
                    "between two photographs a second apart)\n", pc);
            rc = 3;
            goto out;
        }
        rc = 0;
        goto out;
    }

    pc = compare_ppm (argv[2], img, mask, &open, &y0, &y1);
    /* Nothing judged is nothing to compare, which the file above calls 3, and
       is not the same as a photograph that cannot be used at all */
    if (pc == -2)
    {
        printf ("the desktop is covered (%.0f%% of the screen is left), "
                "so leftovers on it cannot be seen\n", open);
        rc = 3;
        goto out;
    }
    if (pc < 0)
    {
        fprintf (stderr, "no usable photograph of the screen before the load\n");
        rc = 2;
        goto out;
    }
    if (open < MIN_OPEN)
    {
        printf ("the desktop is covered (%.0f%% of the screen is left), "
                "so leftovers on it cannot be seen\n", open);
        rc = 3;
        goto out;
    }
    if (pc > BAD_PC)
    {
        printf ("%.2f%% of the desktop did not come back after the load, "
                "lines %d to %d\n", pc, y0, y1);
        rc = 1;
        goto out;
    }
    printf ("the desktop came back (%.2f%% differs, %.0f%% of the screen "
            "judged)\n", pc, open);
    rc = 0;

out:
    free (mask);
    bw_image_free (img);
    XCloseDisplay (d);
    bw_close ();

    return rc;
}
