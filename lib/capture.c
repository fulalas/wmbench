#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "capture.h"

unsigned long bw_pixel (const bw_image *img, int x, int y)
{
    if (img->rgb != NULL)
    {
        const unsigned char *p = img->rgb + ((size_t) y * img->width + x) * 3;

        return ((unsigned long) p[0] << 16) |
               ((unsigned long) p[1] << 8) | p[2];
    }

    return XGetPixel ((XImage *) img->xim, x, y) & 0xffffff;
}

void bw_image_free (bw_image *img)
{
    if (img == NULL)
    {
        return;
    }
    if (img->xim != NULL)
    {
        XDestroyImage ((XImage *) img->xim);
    }
    free (img->rgb);
    free (img);
}

bw_image *capture_wrap_ximage (void *xim)
{
    bw_image *img;

    if (xim == NULL)
    {
        return NULL;
    }
    img = calloc (1, sizeof *img);
    if (img == NULL)
    {
        XDestroyImage ((XImage *) xim);

        return NULL;
    }
    img->xim = xim;
    img->width = ((XImage *) xim)->width;
    img->height = ((XImage *) xim)->height;

    return img;
}

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

static bw_image *region_from_ppm (const char *path, int x, int y, int w, int h,
                                  int scale, int screen_w, int screen_h)
{
    FILE *f;
    bw_image *img = NULL;
    unsigned char *row = NULL, *data = NULL;
    int sw, sh, maxval, ry, rx;
    int px = x * scale, py = y * scale;

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
    /*
     * The photograph has to be of the screen the caller's coordinates come
     * from. A screenshot command can answer with some other output, or with
     * the whole layout of several, and the bounds test below would then pass
     * on an image the region means nothing in. A check that quietly looks at
     * the wrong place is worse than one that says it could not look, and no
     * image is what the callers read as could not run.
     */
    if (sw != screen_w || sh != screen_h)
    {
        goto out;
    }
    /* The region must be on the screen the command photographed */
    if (px < 0 || py < 0 || px + w * scale > sw || py + h * scale > sh)
    {
        goto out;
    }

    data = malloc ((size_t) w * h * 3);
    row = malloc ((size_t) sw * 3);
    if (data == NULL || row == NULL)
    {
        goto out;
    }

    for (ry = 0; ry < py + h * scale; ry++)
    {
        if (fread (row, 3, (size_t) sw, f) != (size_t) sw)
        {
            goto out;
        }
        if (ry < py || (ry - py) % scale != 0)
        {
            continue;
        }
        for (rx = 0; rx < w; rx++)
        {
            memcpy (data + ((size_t) ((ry - py) / scale) * w + rx) * 3,
                    row + (size_t) (px + rx * scale) * 3, 3);
        }
    }

    img = calloc (1, sizeof *img);
    if (img != NULL)
    {
        img->width = w;
        img->height = h;
        img->rgb = data;
        data = NULL;
    }

out:
    free (data);
    free (row);
    fclose (f);

    return img;
}

bw_image *capture_via_cmd (int x, int y, int w, int h, int scale,
                           int screen_w, int screen_h)
{
    const char *cmd = getenv ("BENCH_CAPTURE_CMD");
    char path[64], full[1024];
    bw_image *img;

    if (cmd == NULL || cmd[0] == '\0')
    {
        return NULL;
    }
    snprintf (path, sizeof path, "/tmp/bench-capture-%ld.ppm", (long) getpid ());
    snprintf (full, sizeof full, "%s %s", cmd, path);
    if (system (full) != 0)
    {
        unlink (path);

        return NULL;
    }
    img = region_from_ppm (path, x, y, w, h, scale, screen_w, screen_h);
    unlink (path);

    return img;
}

int capture_write_ppm (const char *path, const bw_image *img)
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
            unsigned long p = bw_pixel (img, x, y);

            row[x * 3] = (p >> 16) & 0xff;
            row[x * 3 + 1] = (p >> 8) & 0xff;
            row[x * 3 + 2] = p & 0xff;
        }
        ok = fwrite (row, 3, (size_t) img->width, f) == (size_t) img->width;
    }
    free (row);

    return fclose (f) == 0 && ok;
}
