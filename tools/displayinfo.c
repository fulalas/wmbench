/*
 * What the screen really is, asked of the compositor over wl_output, which
 * every compositor has. Prints "width height refresh-mHz scale" and nothing
 * else; the caller turns that into a line. Silence means no answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "protocols/xdg-output-unstable-v1-client-protocol.h"

#define MAX_OUTS 8

typedef struct {
    struct wl_output *wl;
    char name[64];
    int scale;
    int mode_w, mode_h;
    int refresh;                /* mHz, 0 where there is no real panel */
    int lw, lh;
    int have_logical;
} out_info;

static out_info outs[MAX_OUTS];
static int nouts;
static struct zxdg_output_manager_v1 *out_mgr;

static void out_geometry (void *data, struct wl_output *o, int32_t x, int32_t y,
                          int32_t pw, int32_t ph, int32_t sub, const char *make,
                          const char *model, int32_t transform)
{
    (void) data; (void) o; (void) x; (void) y; (void) pw; (void) ph;
    (void) sub; (void) make; (void) model; (void) transform;
}

static void out_mode (void *data, struct wl_output *o, uint32_t flags,
                      int32_t w, int32_t h, int32_t refresh)
{
    out_info *oi = data;

    (void) o;
    if (flags & WL_OUTPUT_MODE_CURRENT)
    {
        oi->mode_w = w;
        oi->mode_h = h;
        oi->refresh = refresh;
    }
}

static void out_done (void *data, struct wl_output *o)
{
    (void) data; (void) o;
}

static void out_scale (void *data, struct wl_output *o, int32_t s)
{
    out_info *oi = data;

    (void) o;
    oi->scale = (s > 0) ? s : 1;
}

static void out_name (void *data, struct wl_output *o, const char *name)
{
    out_info *oi = data;

    (void) o;
    snprintf (oi->name, sizeof oi->name, "%s", name);
}

static void out_desc (void *data, struct wl_output *o, const char *desc)
{
    (void) data; (void) o; (void) desc;
}

static const struct wl_output_listener out_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_desc
};

static void xout_pos (void *data, struct zxdg_output_v1 *xo,
                      int32_t x, int32_t y)
{
    (void) data; (void) xo; (void) x; (void) y;
}

static void xout_size (void *data, struct zxdg_output_v1 *xo,
                       int32_t w, int32_t h)
{
    out_info *oi = data;

    (void) xo;
    oi->lw = w;
    oi->lh = h;
    oi->have_logical = 1;
}

static void xout_done (void *data, struct zxdg_output_v1 *xo)
{
    (void) data; (void) xo;
}

static void xout_name (void *data, struct zxdg_output_v1 *xo, const char *n)
{
    (void) data; (void) xo; (void) n;
}

static void xout_desc (void *data, struct zxdg_output_v1 *xo, const char *d)
{
    (void) data; (void) xo; (void) d;
}

static const struct zxdg_output_v1_listener xout_listener = {
    xout_pos, xout_size, xout_done, xout_name, xout_desc
};

static void reg_global (void *data, struct wl_registry *r, uint32_t id,
                        const char *iface, uint32_t ver)
{
    (void) data;
    if (strcmp (iface, wl_output_interface.name) == 0 && nouts < MAX_OUTS)
    {
        out_info *oi = &outs[nouts++];
        uint32_t want = (ver < 4) ? ver : 4;

        oi->scale = 1;
        oi->wl = wl_registry_bind (r, id, &wl_output_interface, want);
        wl_output_add_listener (oi->wl, &out_listener, oi);
    }
    else if (strcmp (iface, zxdg_output_manager_v1_interface.name) == 0)
    {
        out_mgr = wl_registry_bind (r, id, &zxdg_output_manager_v1_interface,
                                    (ver < 2) ? ver : 2);
    }
}

static void reg_remove (void *data, struct wl_registry *r, uint32_t id)
{
    (void) data; (void) r; (void) id;
}

static const struct wl_registry_listener reg_listener = {
    reg_global, reg_remove
};

int main (void)
{
    struct wl_display *dpy;
    struct wl_registry *registry;
    out_info *oi;
    const char *want;
    double scale;
    int i;

    dpy = wl_display_connect (NULL);
    if (dpy == NULL)
    {
        return 1;
    }
    registry = wl_display_get_registry (dpy);
    wl_registry_add_listener (registry, &reg_listener, NULL);
    wl_display_roundtrip (dpy);
    /* the outputs' own events */
    wl_display_roundtrip (dpy);
    if (nouts == 0)
    {
        wl_display_disconnect (dpy);

        return 1;
    }
    if (out_mgr != NULL)
    {
        for (i = 0; i < nouts; i++)
        {
            struct zxdg_output_v1 *xo =
                zxdg_output_manager_v1_get_xdg_output (out_mgr, outs[i].wl);

            zxdg_output_v1_add_listener (xo, &xout_listener, &outs[i]);
        }
        wl_display_roundtrip (dpy);
    }

    /* The one the loads run on: same choice lib/win_wl.c makes */
    oi = &outs[0];
    want = getenv ("BENCH_OUTPUT");
    if (want != NULL && want[0] != '\0')
    {
        for (i = 0; i < nouts; i++)
        {
            if (strcmp (outs[i].name, want) == 0)
            {
                oi = &outs[i];
            }
        }
    }
    if (oi->mode_w <= 0 || oi->mode_h <= 0)
    {
        wl_display_disconnect (dpy);

        return 1;
    }
    /* The logical size carries a fractional scale the integer one cannot */
    scale = oi->scale;
    if (oi->have_logical && oi->lw > 0)
    {
        scale = (double) oi->mode_w / oi->lw;
    }
    printf ("%d %d %d %.2f\n", oi->mode_w, oi->mode_h, oi->refresh, scale);
    wl_display_disconnect (dpy);

    return 0;
}
