#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "capture.h"

/* One PPM (P6) header token, skipping whitespace and # comments */
static int ppm_token (FILE *f)
{
    int c, v = 0;

    do
    {
        c = fgetc (f);
        if (c == '#')
        {
            while (c != '\n' && c != EOF)
            {
                c = fgetc (f);
            }
        }
    } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');

    if (c < '0' || c > '9')
    {
        return -1;
    }
    while (c >= '0' && c <= '9')
    {
        v = v * 10 + (c - '0');
        c = fgetc (f);
    }

    return v;
}

static XImage *region_from_ppm (Display *d, const char *path,
                                int x, int y, unsigned int w, unsigned int h)
{
    FILE *f;
    XImage *img = NULL;
    unsigned char *row = NULL;
    char *data;
    int sw, sh, maxval, ry;
    unsigned int rx;

    f = fopen (path, "rb");
    if (f == NULL)
    {
        return NULL;
    }
    if (fgetc (f) != 'P' || fgetc (f) != '6')
    {
        goto out;
    }
    sw = ppm_token (f);
    sh = ppm_token (f);
    maxval = ppm_token (f);
    if (sw <= 0 || sh <= 0 || maxval != 255)
    {
        goto out;
    }
    /* The region must be on the screen the command photographed */
    if (x < 0 || y < 0 || x + (int) w > sw || y + (int) h > sh)
    {
        goto out;
    }

    data = calloc ((size_t) w * h, 4);
    row = malloc ((size_t) sw * 3);
    if (data == NULL || row == NULL)
    {
        free (data);
        goto out;
    }
    img = XCreateImage (d, DefaultVisual (d, DefaultScreen (d)), 24, ZPixmap,
                        0, data, w, h, 32, 0);
    if (img == NULL)
    {
        free (data);
        goto out;
    }

    for (ry = 0; ry < y + (int) h; ry++)
    {
        if (fread (row, 3, (size_t) sw, f) != (size_t) sw)
        {
            XDestroyImage (img);
            img = NULL;
            goto out;
        }
        if (ry < y)
        {
            continue;
        }
        for (rx = 0; rx < w; rx++)
        {
            const unsigned char *p = row + (size_t) (x + (int) rx) * 3;

            XPutPixel (img, (int) rx, ry - y,
                       ((unsigned long) p[0] << 16) |
                       ((unsigned long) p[1] << 8) | p[2]);
        }
    }

out:
    free (row);
    fclose (f);

    return img;
}

int capture_write_ppm (const char *path, XImage *img)
{
    FILE *f;
    unsigned char *row;
    int x, y, ok = 1;

    f = fopen (path, "wb");
    if (f == NULL)
    {
        return 0;
    }
    row = malloc ((size_t) img->width * 3);
    if (row == NULL)
    {
        fclose (f);

        return 0;
    }
    fprintf (f, "P6\n%d %d\n255\n", img->width, img->height);
    for (y = 0; y < img->height && ok; y++)
    {
        for (x = 0; x < img->width; x++)
        {
            unsigned long p = XGetPixel (img, x, y);

            row[x * 3] = (p >> 16) & 0xff;
            row[x * 3 + 1] = (p >> 8) & 0xff;
            row[x * 3 + 2] = p & 0xff;
        }
        ok = fwrite (row, 3, (size_t) img->width, f) == (size_t) img->width;
    }
    free (row);

    return fclose (f) == 0 && ok;
}

/* A failed grab must come back as no image, not as a killed process */
static int swallow_x_error (Display *dd, XErrorEvent *e)
{
    (void) dd;
    (void) e;

    return 0;
}

XImage *capture_region (Display *d, Window root,
                        int x, int y, unsigned int w, unsigned int h)
{
    const char *cmd = getenv ("BENCH_CAPTURE_CMD");
    char path[64], full[1024];
    XImage *img;

    if (cmd == NULL || cmd[0] == '\0')
    {
        int (*old_handler) (Display *, XErrorEvent *);

        XSync (d, False);
        old_handler = XSetErrorHandler (swallow_x_error);
        img = XGetImage (d, root, x, y, w, h, AllPlanes, ZPixmap);
        XSync (d, False);
        XSetErrorHandler (old_handler);

        return img;
    }

    snprintf (path, sizeof path, "/tmp/bench-capture-%ld.ppm", (long) getpid ());
    snprintf (full, sizeof full, "%s %s", cmd, path);
    if (system (full) != 0)
    {
        unlink (path);

        return NULL;
    }
    img = region_from_ppm (d, path, x, y, w, h);
    unlink (path);

    return img;
}
