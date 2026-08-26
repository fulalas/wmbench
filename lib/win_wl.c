/*
 * Where a global is missing, the calls that need it say so by their return
 * value and the tools fire the suite's usual refusal channels. Nothing here
 * may pretend: a window that cannot be placed reports itself unplaceable
 * rather than believing coordinates nobody honoured.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "win_priv.h"
#include "draw.h"
#include "protocols/xdg-shell-client-protocol.h"
#include "protocols/wlr-layer-shell-unstable-v1-client-protocol.h"
#include "protocols/xdg-activation-v1-client-protocol.h"
#include "protocols/alpha-modifier-v1-client-protocol.h"
#include "protocols/xdg-output-unstable-v1-client-protocol.h"
#include "protocols/xdg-decoration-unstable-v1-client-protocol.h"
#include "protocols/xx-zones-v1-client-protocol.h"
#include "protocols/idle-inhibit-unstable-v1-client-protocol.h"
#include "protocols/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define STAGE_W 1920
#define STAGE_H 1080
#define MAX_BUFS 8

static struct wl_display *dpy;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_subcompositor *subcompositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct xdg_activation_v1 *activation;
static struct wp_alpha_modifier_v1 *alpha_mod;
static struct zxdg_output_manager_v1 *out_mgr;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct zwp_idle_inhibit_manager_v1 *idle_mgr;
/* Unlike layer-shell margins, zones answer: the compositor reports every
   item's position back, the way the X server answers XTranslateCoordinates */
static struct xx_zone_manager_v1 *zone_mgr;
static struct xx_zone_v1 *zone;
static int zone_done, zone_bad;
static struct zwlr_foreign_toplevel_manager_v1 *ftl_mgr;
static struct wl_seat *seat;

typedef struct {
    struct wl_output *wl;
    char name[64];
    int scale;                  /* integer buffer scale */
    int mode_w, mode_h;         /* pixels on the panel */
    int lx, ly, lw, lh;         /* the logical box in the layout */
    int have_logical;
} wl_out;

#define MAX_OUTS 8
static wl_out outs[MAX_OUTS];
static int nouts;
static wl_out *out;             /* the chosen one */

typedef enum { K_TOPLEVEL, K_LAYER, K_XDG_POPUP, K_SUBSURFACE,
               K_UNMAPPABLE } wl_kind;

typedef struct {
    struct wl_shm_pool *pool;
    struct wl_buffer *wb;
    unsigned int *px;
    size_t cap;                 /* what the pool holds; grows, never shrinks */
    int w, h;                   /* buffer pixels */
    int busy;
    unsigned seq;               /* the present it was last attached at; 0 never */
} wl_buf;

/* Where the raster was dirty at each recent present, so an older buffer is
   brought up to date by copying only what it missed: sending whole windows
   instead would put a cost in the Wayland rows that no X11 row carries */
#define DMG_KEEP 16
typedef struct { int x0, y0, x1, y1; } dmg_box;

typedef struct {
    struct wl_surface *surf;
    struct xdg_surface *xsurf;
    struct xdg_toplevel *toplevel;
    struct zwlr_layer_surface_v1 *layer;
    struct xdg_popup *popup;
    struct xdg_positioner *positioner;
    struct wp_alpha_modifier_surface_v1 *alpha;
    struct zxdg_toplevel_decoration_v1 *deco;
    struct xx_zone_item_v1 *zitem;
    struct wl_subsurface *subsurf;
    struct zwp_idle_inhibitor_v1 *idle;
    int use_zone;
    int zx, zy, zvalid;         /* the compositor's own account, once given */
    wl_kind kind;
    int mapped;
    int configured;
    unsigned states;            /* from the last toplevel configure */
    int conf_w, conf_h;         /* what the compositor asked for, logical */
    int gl;

    draw_buf buf;               /* the content, in buffer pixels */
    unsigned int *raster;
    size_t raster_cap;          /* bytes held; grows once, never per resize */
    int bw, bh;
    int ever_presented;
    wl_buf bufs[MAX_BUFS];
    dmg_box dmg[DMG_KEEP];
    unsigned dmg_seq;           /* the next present's number, from 1 */
    int frame_mode;             /* whole frames straight into buffers */
    wl_buf *frame_out;

    bw_win *bg_canvas;
    unsigned long bg_colour;
    int want_fullscreen;
    int want_maximized;
    double alpha_value;
    int have_alpha_value;
    int opq_x, opq_y, opq_w, opq_h, have_opq;
    char token[256];            /* an activation token made while still shown */
} wl_win;

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
    wl_out *ow = data;

    (void) o; (void) refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT)
    {
        ow->mode_w = w;
        ow->mode_h = h;
    }
}

static void out_done (void *data, struct wl_output *o)
{
    (void) data; (void) o;
}

static void out_scale (void *data, struct wl_output *o, int32_t s)
{
    wl_out *ow = data;

    (void) o;
    ow->scale = (s > 0) ? s : 1;
}

static void out_name (void *data, struct wl_output *o, const char *name)
{
    wl_out *ow = data;

    (void) o;
    snprintf (ow->name, sizeof ow->name, "%s", name);
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
    wl_out *ow = data;

    (void) xo;
    ow->lx = x;
    ow->ly = y;
    ow->have_logical = 1;
}

static void xout_size (void *data, struct zxdg_output_v1 *xo,
                       int32_t w, int32_t h)
{
    wl_out *ow = data;

    (void) xo;
    ow->lw = w;
    ow->lh = h;
    ow->have_logical = 1;
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

/* Answer the pings or be disconnected mid-run */
static void wm_ping (void *data, struct xdg_wm_base *b, uint32_t serial)
{
    (void) data;
    xdg_wm_base_pong (b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_ping };

typedef struct {
    struct zwlr_foreign_toplevel_handle_v1 *h;
    char title[128];
    int gone;
} ftl_win;

#define MAX_FTL 128
static ftl_win ftls[MAX_FTL];
static int nftls;

static void ftl_title (void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                       const char *title)
{
    ftl_win *f = data;

    (void) h;
    snprintf (f->title, sizeof f->title, "%s", title);
}

static void ftl_app_id (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                        const char *a) { (void) d; (void) h; (void) a; }
static void ftl_out_enter (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                           struct wl_output *o) { (void) d; (void) h; (void) o; }
static void ftl_out_leave (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                           struct wl_output *o) { (void) d; (void) h; (void) o; }
static void ftl_state (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                       struct wl_array *s) { (void) d; (void) h; (void) s; }
static void ftl_done (void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{ (void) d; (void) h; }

static void ftl_closed (void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    ftl_win *f = data;

    f->gone = 1;
    zwlr_foreign_toplevel_handle_v1_destroy (h);
    f->h = NULL;
}

static void ftl_parent (void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                        struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void) d; (void) h; (void) p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener ftl_listener = {
    ftl_title, ftl_app_id, ftl_out_enter, ftl_out_leave, ftl_state,
    ftl_done, ftl_closed, ftl_parent
};

static void ftl_new (void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                     struct zwlr_foreign_toplevel_handle_v1 *h)
{
    int i;

    (void) data; (void) m;
    for (i = 0; i < MAX_FTL; i++)
    {
        if (i == nftls || ftls[i].gone || ftls[i].h == NULL)
        {
            break;
        }
    }
    if (i == MAX_FTL)
    {
        zwlr_foreign_toplevel_handle_v1_destroy (h);

        return;
    }
    if (i == nftls)
    {
        nftls++;
    }
    memset (&ftls[i], 0, sizeof ftls[i]);
    ftls[i].h = h;
    zwlr_foreign_toplevel_handle_v1_add_listener (h, &ftl_listener, &ftls[i]);
}

static void ftl_finished (void *d, struct zwlr_foreign_toplevel_manager_v1 *m)
{ (void) d; (void) m; }

static const struct zwlr_foreign_toplevel_manager_v1_listener ftl_mgr_listener = {
    ftl_new, ftl_finished
};

static void reg_global (void *data, struct wl_registry *r, uint32_t id,
                        const char *iface, uint32_t ver)
{
    (void) data;
    if (strcmp (iface, wl_compositor_interface.name) == 0)
    {
        compositor = wl_registry_bind (r, id, &wl_compositor_interface,
                                       ver < 4 ? ver : 4);
    }
    else if (strcmp (iface, wl_shm_interface.name) == 0)
    {
        shm = wl_registry_bind (r, id, &wl_shm_interface, 1);
    }
    else if (strcmp (iface, wl_subcompositor_interface.name) == 0)
    {
        subcompositor = wl_registry_bind (r, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp (iface, xdg_wm_base_interface.name) == 0)
    {
        wm_base = wl_registry_bind (r, id, &xdg_wm_base_interface,
                                    ver < 2 ? 1 : 2);
        xdg_wm_base_add_listener (wm_base, &wm_base_listener, NULL);
    }
    else if (strcmp (iface, zwlr_layer_shell_v1_interface.name) == 0)
    {
        layer_shell = wl_registry_bind (r, id, &zwlr_layer_shell_v1_interface,
                                        ver < 2 ? 1 : 2);
    }
    else if (strcmp (iface, xdg_activation_v1_interface.name) == 0)
    {
        activation = wl_registry_bind (r, id, &xdg_activation_v1_interface, 1);
    }
    else if (strcmp (iface, wp_alpha_modifier_v1_interface.name) == 0)
    {
        alpha_mod = wl_registry_bind (r, id, &wp_alpha_modifier_v1_interface, 1);
    }
    else if (strcmp (iface, zxdg_output_manager_v1_interface.name) == 0)
    {
        out_mgr = wl_registry_bind (r, id, &zxdg_output_manager_v1_interface,
                                    ver < 2 ? 1 : 2);
    }
    else if (strcmp (iface, zxdg_decoration_manager_v1_interface.name) == 0)
    {
        deco_mgr = wl_registry_bind (r, id,
                                     &zxdg_decoration_manager_v1_interface, 1);
    }
    else if (strcmp (iface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
    {
        idle_mgr = wl_registry_bind (r, id,
                                     &zwp_idle_inhibit_manager_v1_interface, 1);
    }
    else if (strcmp (iface, xx_zone_manager_v1_interface.name) == 0)
    {
        zone_mgr = wl_registry_bind (r, id, &xx_zone_manager_v1_interface, 1);
    }
    else if (strcmp (iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0)
    {
        ftl_mgr = wl_registry_bind (r, id,
                                    &zwlr_foreign_toplevel_manager_v1_interface,
                                    1);
        zwlr_foreign_toplevel_manager_v1_add_listener (ftl_mgr,
                                                       &ftl_mgr_listener, NULL);
    }
    else if (strcmp (iface, wl_seat_interface.name) == 0 && seat == NULL)
    {
        seat = wl_registry_bind (r, id, &wl_seat_interface, 1);
    }
    else if (strcmp (iface, wl_output_interface.name) == 0 && nouts < MAX_OUTS)
    {
        wl_out *ow = &outs[nouts++];

        ow->scale = 1;
        ow->wl = wl_registry_bind (r, id, &wl_output_interface,
                                   ver < 4 ? ver : 4);
        wl_output_add_listener (ow->wl, &out_listener, ow);
    }
}

static void reg_gone (void *data, struct wl_registry *r, uint32_t id)
{
    (void) data; (void) r; (void) id;
}

static const struct wl_registry_listener reg_listener = {
    reg_global, reg_gone
};

/*
 * Wait for events, but never for ever. wl_display_dispatch() sleeps in poll
 * until something arrives, so a loop counted in iterations around it is not a
 * timeout at all: a compositor that simply never answers - which is exactly
 * what a refused activation looks like - would hold a benchmark for the rest
 * of the run, with the script outside waiting on a mark that never comes.
 * Returns 0 when the time ran out.
 */
static int dispatch_for (int ms)
{
    struct pollfd p;
    int left = ms;

    while (left > 0)
    {
        struct timespec a, b;
        int got;

        wl_display_dispatch_pending (dpy);
        wl_display_flush (dpy);
        while (wl_display_prepare_read (dpy) != 0)
        {
            int n = wl_display_dispatch_pending (dpy);

            if (n > 0)
            {
                return 1;
            }
            /*
             * A failure leaves the queue undrained and the error latched, so
             * prepare_read goes on refusing: without this the loop spins on a
             * dead connection for the rest of the run.
             */
            if (n < 0)
            {
                return 0;
            }
        }
        p.fd = wl_display_get_fd (dpy);
        p.events = POLLIN;
        p.revents = 0;
        clock_gettime (CLOCK_MONOTONIC, &a);
        got = poll (&p, 1, left);
        clock_gettime (CLOCK_MONOTONIC, &b);
        if (got > 0)
        {
            if (wl_display_read_events (dpy) < 0)
            {
                return 0;
            }
            wl_display_dispatch_pending (dpy);

            return 1;
        }
        wl_display_cancel_read (dpy);
        if (got < 0 && errno == EINTR)
        {
            int spent = (int) ((b.tv_sec - a.tv_sec) * 1000 +
                               (b.tv_nsec - a.tv_nsec) / 1000000);

            /* Sub-millisecond interruptions round down to no time spent, and
               a stream of them would make this timeout eternal */
            left -= (spent > 0) ? spent : 1;
            continue;
        }

        return 0;
    }

    return 0;
}

static void zone_size (void *data, struct xx_zone_v1 *z, int32_t w, int32_t h)
{
    (void) data; (void) z;
    if (w < 0 && h < 0)
    {
        zone_bad = 1;
    }
}

static void zone_handle (void *data, struct xx_zone_v1 *z, const char *handle)
{
    (void) data; (void) z; (void) handle;
}

static void zone_done_ (void *data, struct xx_zone_v1 *z)
{
    (void) data; (void) z;
    zone_done = 1;
}

static void zone_item_blocked (void *data, struct xx_zone_v1 *z,
                               struct xx_zone_item_v1 *item)
{
    (void) data; (void) z; (void) item;
}

static void zone_item_entered (void *data, struct xx_zone_v1 *z,
                               struct xx_zone_item_v1 *item)
{
    (void) data; (void) z; (void) item;
}

static void zone_item_left (void *data, struct xx_zone_v1 *z,
                            struct xx_zone_item_v1 *item)
{
    (void) data; (void) z; (void) item;
}

static const struct xx_zone_v1_listener zone_listener = {
    zone_size, zone_handle, zone_done_,
    zone_item_blocked, zone_item_entered, zone_item_left
};

static void zitem_frame_extents (void *data, struct xx_zone_item_v1 *item,
                                 int32_t top, int32_t bottom,
                                 int32_t left, int32_t right)
{
    (void) data; (void) item;
    (void) top; (void) bottom; (void) left; (void) right;
}

static void zitem_position (void *data, struct xx_zone_item_v1 *item,
                            int32_t x, int32_t y)
{
    bw_win *win = data;
    wl_win *ww = win->impl;

    (void) item;
    ww->zx = x;
    ww->zy = y;
    ww->zvalid = 1;
}

static void zitem_position_failed (void *data, struct xx_zone_item_v1 *item)
{
    (void) data; (void) item;
    /* Nothing moved: the next look at bw_where says so, the way a refused
       X11 move leaves XTranslateCoordinates unimpressed */
}

static void zitem_closed (void *data, struct xx_zone_item_v1 *item)
{
    bw_win *win = data;
    wl_win *ww = win->impl;

    (void) item;
    ww->zvalid = 0;
}

static const struct xx_zone_item_v1_listener zitem_listener = {
    zitem_frame_extents, zitem_position, zitem_position_failed, zitem_closed
};

static int wl_open (void)
{
    const char *want;
    int i;

    dpy = wl_display_connect (NULL);
    if (dpy == NULL)
    {
        return 0;
    }
    registry = wl_display_get_registry (dpy);
    wl_registry_add_listener (registry, &reg_listener, NULL);
    wl_display_roundtrip (dpy);
    /* the outputs' own events, and the foreign toplevels' titles */
    wl_display_roundtrip (dpy);

    if (compositor == NULL || shm == NULL || wm_base == NULL || nouts == 0)
    {
        wl_display_disconnect (dpy);
        dpy = NULL;

        return 0;
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

    out = &outs[0];
    want = getenv ("BENCH_OUTPUT");
    if (want != NULL && want[0] != '\0')
    {
        for (i = 0; i < nouts; i++)
        {
            if (strcmp (outs[i].name, want) == 0)
            {
                out = &outs[i];
            }
        }
    }
    if (!out->have_logical)
    {
        out->lx = 0;
        out->ly = 0;
        out->lw = out->mode_w / out->scale;
        out->lh = out->mode_h / out->scale;
    }
    if (out->lw <= 0 || out->lh <= 0)
    {
        wl_display_disconnect (dpy);
        dpy = NULL;

        return 0;
    }

    if (zone_mgr != NULL)
    {
        int i;

        zone = xx_zone_manager_v1_get_zone (zone_mgr, out->wl);
        xx_zone_v1_add_listener (zone, &zone_listener, NULL);
        for (i = 0; i < 20 && !zone_done; i++)
        {
            if (!dispatch_for (100))
            {
                break;
            }
        }
        if (zone_bad || !zone_done)
        {
            xx_zone_v1_destroy (zone);
            zone = NULL;
        }
    }

    return 1;
}

static void wl_close (void)
{
    wl_display_disconnect (dpy);
    dpy = NULL;
}

/*
 * Fractional scaling: the panel has more pixels than logical size times the
 * integer scale accounts for, so no shm buffer of ours can map 1:1 onto a
 * screenshot. Every capture then honestly answers could-not-look.
 */
static int fractional (void)
{
    return out->lw * out->scale != out->mode_w ||
           out->lh * out->scale != out->mode_h;
}

static void wl_screen_size (int *w, int *h)
{
    if (w != NULL) *w = out->lw;
    if (h != NULL) *h = out->lh;
}

static void wl_stage (int margin, int *x, int *y, int *w, int *h)
{
    int stage_w = (out->lw < STAGE_W) ? out->lw : STAGE_W;
    int stage_h = (out->lh < STAGE_H) ? out->lh : STAGE_H;

    *x = out->lx + (out->lw - stage_w) / 2 + margin;
    *y = out->ly + (out->lh - stage_h) / 2 + margin;
    *w = stage_w - 2 * margin;
    *h = stage_h - 2 * margin;
    if (*w < 1)
    {
        *w = 1;
    }
    if (*h < 1)
    {
        *h = 1;
    }
}

static void wl_sync_ (void)
{
    wl_display_roundtrip (dpy);
}

static void wl_pump_ (void)
{
    /*
     * Rate limited: the pacing loops call this every 200 us, and a poll five
     * thousand times a second is CPU the equivalent X11 loop, a bare usleep,
     * never spends. A couple of milliseconds is fast enough for a ping.
     */
    static struct timespec last;
    struct timespec now;
    struct pollfd p;

    clock_gettime (CLOCK_MONOTONIC, &now);
    if (last.tv_sec != 0 &&
        (now.tv_sec - last.tv_sec) * 1000000000L +
        (now.tv_nsec - last.tv_nsec) < 2000000L)
    {
        return;
    }
    last = now;

    wl_display_dispatch_pending (dpy);
    wl_display_flush (dpy);
    while (wl_display_prepare_read (dpy) != 0)
    {
        /* Same as dispatch_for: a dead connection never drains, and this is
           called every 200 us for the length of a measured run */
        if (wl_display_dispatch_pending (dpy) < 0)
        {
            return;
        }
    }
    p.fd = wl_display_get_fd (dpy);
    p.events = POLLIN;
    p.revents = 0;
    if (poll (&p, 1, 0) > 0)
    {
        wl_display_read_events (dpy);
    }
    else
    {
        wl_display_cancel_read (dpy);
    }
    wl_display_dispatch_pending (dpy);
}

static void buf_release (void *data, struct wl_buffer *b)
{
    wl_buf *wb = data;

    (void) b;
    wb->busy = 0;
}

static const struct wl_buffer_listener buf_listener = { buf_release };

static void buf_drop (wl_buf *b)
{
    if (b->wb != NULL)
    {
        wl_buffer_destroy (b->wb);
    }
    if (b->pool != NULL)
    {
        wl_shm_pool_destroy (b->pool);
        munmap (b->px, b->cap);
    }
    memset (b, 0, sizeof *b);
}

/*
 * A buffer at this size, recycled. The pool behind each one grows and is
 * kept: a resizing load asks for a new size sixty times a second, and a
 * memfd, an mmap and an munmap on every step is a syscall bill the X11
 * client - which sends one resize request and draws nothing - never pays.
 * Only the wl_buffer, a protocol object, is remade when the size changes.
 */
static wl_buf *buf_get (wl_win *ww, int w, int h)
{
    int i, tries;

    for (tries = 0; tries < 1000; tries++)
    {
        wl_buf *spare = NULL;

        for (i = 0; i < MAX_BUFS; i++)
        {
            wl_buf *b = &ww->bufs[i];

            if (b->busy)
            {
                continue;
            }
            if (b->wb != NULL && b->w == w && b->h == h)
            {
                return b;
            }
            if (spare == NULL || (spare->pool == NULL && b->pool != NULL))
            {
                spare = b;
            }
        }
        if (spare != NULL)
        {
            size_t size = (size_t) w * h * 4;

            if (spare->pool == NULL || spare->cap < size)
            {
                /* Rounded well past what this frame needs: a growing resize
                   asks for a larger buffer every step, and an exact fit
                   meant a memfd, an mmap and a pool round trip per frame */
                size_t want = size + size / 2;
                int fd;

                buf_drop (spare);
                fd = memfd_create ("wmbench", MFD_CLOEXEC);
                if (fd < 0 || ftruncate (fd, (off_t) want) != 0)
                {
                    if (fd >= 0)
                    {
                        close (fd);
                    }

                    return NULL;
                }
                spare->px = mmap (NULL, want, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
                if (spare->px == MAP_FAILED)
                {
                    close (fd);
                    spare->px = NULL;

                    return NULL;
                }
                spare->pool = wl_shm_create_pool (shm, fd, (int32_t) want);
                spare->cap = want;
                close (fd);
            }
            else if (spare->wb != NULL)
            {
                wl_buffer_destroy (spare->wb);
                spare->wb = NULL;
            }
            spare->wb = wl_shm_pool_create_buffer (spare->pool, 0, w, h, w * 4,
                                                   (ww->gl == 0 &&
                                                    (ww->buf.argb))
                                                   ? WL_SHM_FORMAT_ARGB8888
                                                   : WL_SHM_FORMAT_XRGB8888);
            spare->w = w;
            spare->h = h;
            spare->seq = 0;
            wl_buffer_add_listener (spare->wb, &buf_listener, spare);

            return spare;
        }
        /* Every buffer is with the compositor; wait for one to come back */
        wl_display_roundtrip (dpy);
    }

    return NULL;
}

/* The background - a colour, or the canvas tiled - into one patch of the
   raster, which is what the server does to every strip a window gains */
static void raster_fill_bg (bw_win *win, draw_buf *b,
                            int x, int y, int w, int h)
{
    wl_win *ww = win->impl;

    if (ww->bg_canvas != NULL)
    {
        wl_win *cv = ww->bg_canvas->impl;
        int tx, ty;

        draw_clip (b, x, y, w, h);
        for (ty = (y / cv->bh) * cv->bh; ty < y + h; ty += cv->bh)
        {
            for (tx = (x / cv->bw) * cv->bw; tx < x + w; tx += cv->bw)
            {
                draw_copy (b, &cv->buf, 0, 0, cv->bw, cv->bh, tx, ty);
            }
        }
        draw_clip (b, 0, 0, -1, -1);
    }
    else
    {
        draw_fill (b, ww->bg_colour, x, y, w, h);
    }
}

/*
 * The content raster at the window's current size. The allocation only ever
 * grows and the stride is the widest the window has been, so a resize keeps
 * every pixel where it lies - the X11 server's own retained-pixmap economy -
 * and only the strips the window gained are filled, from the background,
 * exactly as the server tiles them. Without BW_KEEP the whole window refills
 * instead, which is X11's ForgetGravity and part of what a resizing load
 * measures.
 */
static void raster_ensure (bw_win *win)
{
    wl_win *ww = win->impl;
    int bw = win->w * out->scale, bh = win->h * out->scale;
    int ow = ww->bw, oh = ww->bh;
    int stride = ww->buf.stride > bw ? ww->buf.stride : bw;
    size_t need = (size_t) stride * (bh > oh ? bh : oh) * 4;

    if (ww->raster != NULL && ow == bw && oh == bh)
    {
        return;
    }
    if (ww->raster == NULL || need > ww->raster_cap)
    {
        /* Half as much again, so a resize sweep allocates a handful of times
           instead of once a frame */
        unsigned int *nr = malloc (need + need / 2);

        if (nr == NULL)
        {
            return;
        }
        if (ww->raster != NULL && (win->flags & BW_KEEP))
        {
            int row, keep = (oh < bh) ? oh : bh;

            for (row = 0; row < keep; row++)
            {
                memcpy (nr + (size_t) row * stride,
                        ww->raster + (size_t) row * ww->buf.stride,
                        (size_t) (ow < bw ? ow : bw) * 4);
            }
        }
        free (ww->raster);
        ww->raster = nr;
        ww->raster_cap = need + need / 2;
        if (!(win->flags & BW_KEEP))
        {
            ow = oh = 0;        /* everything is a gained strip now */
        }
    }
    else if (!(win->flags & BW_KEEP))
    {
        ow = oh = 0;
    }
    else if (stride != ww->buf.stride)
    {
        int row, keep = (oh < bh) ? oh : bh;

        /* Backwards, or the widened rows overwrite the ones still to move */
        for (row = keep - 1; row > 0; row--)
        {
            memmove (ww->raster + (size_t) row * stride,
                     ww->raster + (size_t) row * ww->buf.stride,
                     (size_t) (ow < bw ? ow : bw) * 4);
        }
    }
    draw_init (&ww->buf, ww->raster, bw, bh, stride,
               (win->flags & BW_ARGB) != 0);
    ww->bw = bw;
    ww->bh = bh;
    if (bw > ow)
    {
        raster_fill_bg (win, &ww->buf, ow, 0, bw - ow, bh);
    }
    if (bh > oh)
    {
        raster_fill_bg (win, &ww->buf, 0, oh, (bw < ow ? bw : ow), bh - oh);
    }
    /* Everything is new as far as the next commit is concerned */
    draw_damage_all (&ww->buf);
}

static void wl_present_ (bw_win *win)
{
    wl_win *ww = win->impl;
    wl_buf *b;
    dmg_box need;
    int dx, dy, dw, dh, row;
    unsigned k;

    if (win->canvas || ww->surf == NULL || !ww->mapped || ww->gl ||
        ww->frame_mode)
    {
        return;
    }
    raster_ensure (win);
    if (ww->raster == NULL)
    {
        return;
    }
    if (!draw_damaged (&ww->buf, &dx, &dy, &dw, &dh))
    {
        if (ww->ever_presented)
        {
            return;
        }
        dx = 0;
        dy = 0;
        dw = ww->bw;
        dh = ww->bh;
    }
    b = buf_get (ww, ww->bw, ww->bh);
    if (b == NULL)
    {
        return;
    }
    /*
     * What this buffer missed: it was last attached two or three presents
     * ago, so it needs everything drawn since then, not just this frame -
     * but only that, because copying whole windows is a client cost the
     * X11 rows never pay for their one-request draws.
     */
    need.x0 = dx;
    need.y0 = dy;
    need.x1 = dx + dw;
    need.y1 = dy + dh;
    if (b->seq == 0 || ww->dmg_seq - b->seq >= DMG_KEEP)
    {
        need.x0 = 0;
        need.y0 = 0;
        need.x1 = ww->bw;
        need.y1 = ww->bh;
    }
    else
    {
        for (k = b->seq + 1; k <= ww->dmg_seq; k++)
        {
            dmg_box *m = &ww->dmg[k % DMG_KEEP];

            if (m->x0 < need.x0) need.x0 = m->x0;
            if (m->y0 < need.y0) need.y0 = m->y0;
            if (m->x1 > need.x1) need.x1 = m->x1;
            if (m->y1 > need.y1) need.y1 = m->y1;
        }
        if (need.x0 < 0) need.x0 = 0;
        if (need.y0 < 0) need.y0 = 0;
        if (need.x1 > ww->bw) need.x1 = ww->bw;
        if (need.y1 > ww->bh) need.y1 = ww->bh;
    }
    for (row = need.y0; row < need.y1; row++)
    {
        memcpy (b->px + (size_t) row * b->w + need.x0,
                ww->raster + (size_t) row * ww->buf.stride + need.x0,
                (size_t) (need.x1 - need.x0) * 4);
    }
    ww->dmg_seq++;
    ww->dmg[ww->dmg_seq % DMG_KEEP].x0 = dx;
    ww->dmg[ww->dmg_seq % DMG_KEEP].y0 = dy;
    ww->dmg[ww->dmg_seq % DMG_KEEP].x1 = dx + dw;
    ww->dmg[ww->dmg_seq % DMG_KEEP].y1 = dy + dh;
    b->seq = ww->dmg_seq;
    wl_surface_attach (ww->surf, b->wb, 0, 0);
    wl_surface_damage_buffer (ww->surf, dx, dy, dw, dh);
    wl_surface_commit (ww->surf);
    wl_display_flush (dpy);
    b->busy = 1;
    ww->ever_presented = 1;
    draw_damage_reset (&ww->buf);
}

static void xsurf_configure (void *data, struct xdg_surface *s, uint32_t serial)
{
    bw_win *win = data;
    wl_win *ww = win->impl;

    xdg_surface_ack_configure (s, serial);
    ww->configured = 1;
    if (ww->conf_w > 0 && ww->conf_h > 0 &&
        (ww->conf_w != win->w || ww->conf_h != win->h))
    {
        /* The compositor's size wins for a managed window: maximise,
           fullscreen and a snap all arrive here */
        win->w = ww->conf_w;
        win->h = ww->conf_h;
        if (ww->mapped && !ww->gl && !ww->frame_mode)
        {
            raster_ensure (win);
            wl_present_ (win);
        }
    }
}

static const struct xdg_surface_listener xsurf_listener = { xsurf_configure };

static void topl_configure (void *data, struct xdg_toplevel *t,
                            int32_t w, int32_t h, struct wl_array *states)
{
    bw_win *win = data;
    wl_win *ww = win->impl;
    uint32_t *st;
    unsigned got = 0;

    (void) t;
    wl_array_for_each (st, states)
    {
        if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED)
        {
            got |= BW_STATE_MAX;
        }
        if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN)
        {
            got |= BW_STATE_FULLSCREEN;
        }
        if (*st == XDG_TOPLEVEL_STATE_ACTIVATED)
        {
            got |= BW_STATE_ACTIVE;
        }
    }
    ww->states = got;
    ww->conf_w = w;
    ww->conf_h = h;
    /*
     * A fullscreen window covers the output, so for once its place on the
     * screen is known - and that is the whole reason the checks that must
     * aim a capture go fullscreen. It is the output's corner, not the
     * origin of the layout: on a second monitor those differ, and a capture
     * aimed at 0,0 would be cut from somebody else's screen or refused.
     */
    if (got & BW_STATE_FULLSCREEN)
    {
        win->x = out->lx;
        win->y = out->ly;
    }
}

static void topl_close (void *data, struct xdg_toplevel *t)
{
    (void) data; (void) t;
}

static void topl_bounds (void *data, struct xdg_toplevel *t,
                         int32_t w, int32_t h)
{
    (void) data; (void) t; (void) w; (void) h;
}

static void topl_caps (void *data, struct xdg_toplevel *t, struct wl_array *c)
{
    (void) data; (void) t; (void) c;
}

static const struct xdg_toplevel_listener topl_listener = {
    topl_configure, topl_close, topl_bounds, topl_caps
};

static void layer_configure (void *data, struct zwlr_layer_surface_v1 *ls,
                             uint32_t serial, uint32_t w, uint32_t h)
{
    bw_win *win = data;
    wl_win *ww = win->impl;

    (void) w; (void) h;         /* the size asked for is the size kept */
    zwlr_layer_surface_v1_ack_configure (ls, serial);
    ww->configured = 1;
}

static void layer_closed (void *data, struct zwlr_layer_surface_v1 *ls)
{
    bw_win *win = data;
    wl_win *ww = win->impl;

    (void) ls;
    ww->mapped = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    layer_configure, layer_closed
};

static void popup_configure (void *data, struct xdg_popup *p,
                             int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void) data; (void) p; (void) x; (void) y; (void) w; (void) h;
}

static void popup_done (void *data, struct xdg_popup *p)
{
    bw_win *win = data;
    wl_win *ww = win->impl;

    (void) p;
    ww->mapped = 0;
}

static void popup_repositioned (void *data, struct xdg_popup *p, uint32_t tok)
{
    (void) data; (void) p; (void) tok;
}

static const struct xdg_popup_listener popup_listener = {
    popup_configure, popup_done, popup_repositioned
};

static wl_kind kind_of (bw_win *win)
{
    if ((win->flags & BW_CHILD) && win->parent != NULL)
    {
        /* Nothing else here is a child: a toplevel of its own would be a
           window flying about, which is the thing this avoids asking for */
        return subcompositor != NULL ? K_SUBSURFACE : K_UNMAPPABLE;
    }
    if (win->flags & BW_GL)
    {
        return K_TOPLEVEL;
    }
    if ((win->flags & BW_POPUP) && win->parent != NULL)
    {
        return K_XDG_POPUP;
    }
    /*
     * A layer surface is never decorated, so it is only for the windows that
     * carry no decoration on X11 either - menus and drag icons - and for the
     * checks that must aim a capture at a known spot, where being able to
     * look is the whole test. Everything else is a toplevel, which the
     * compositor frames the way a window manager frames an X11 window.
     */
    if ((win->flags & (BW_POPUP | BW_AIMED | BW_UNMANAGED)) &&
        !(win->flags & BW_STATED))
    {
        if (layer_shell != NULL)
        {
            return K_LAYER;
        }
        if (win->flags & BW_POPUP)
        {
            return K_UNMAPPABLE;
        }
    }

    return K_TOPLEVEL;
}

static int wl_create (bw_win *win)
{
    wl_win *ww = calloc (1, sizeof *ww);

    if (ww == NULL)
    {
        return 0;
    }
    ww->kind = kind_of (win);
    /*
     * Zone placement carries what layer-shell cannot: a placed window that is
     * still a managed toplevel, states and all. Not for the windows a capture
     * will be aimed at, though - a zone's origin is opaque by design, so its
     * coordinates are nothing to point a screenshot at.
     */
    ww->use_zone = (ww->kind == K_TOPLEVEL && zone != NULL &&
                    (win->flags & BW_PLACED) && !(win->flags & BW_AIMED));
    ww->gl = (win->flags & BW_GL) != 0;
    ww->bg_colour = 0x000000;
    win->impl = ww;
    draw_init (&ww->buf, NULL, 0, 0, 0, (win->flags & BW_ARGB) != 0);

    return 1;
}

static int wl_canvas_new (bw_win *win)
{
    wl_win *ww = calloc (1, sizeof *ww);

    if (ww == NULL)
    {
        return 0;
    }
    /*
     * In buffer pixels, like a window's raster: everything drawn goes through
     * the same scaling, so a canvas copied into a window lines up 1:1 and the
     * pattern a canvas holds is the pattern the screen shows. Sized in
     * logical pixels instead, every draw into it landed at scaled coordinates
     * in an unscaled buffer and only the top-left corner of the content
     * existed.
     */
    ww->bw = win->w * out->scale;
    ww->bh = win->h * out->scale;
    ww->raster = calloc ((size_t) ww->bw * ww->bh, 4);
    if (ww->raster == NULL)
    {
        free (ww);

        return 0;
    }
    draw_init (&ww->buf, ww->raster, ww->bw, ww->bh, ww->bw, 0);
    win->impl = ww;

    return 1;
}

static void role_teardown (bw_win *win)
{
    wl_win *ww = win->impl;

    if (ww->alpha != NULL)
    {
        wp_alpha_modifier_surface_v1_destroy (ww->alpha);
        ww->alpha = NULL;
    }
    if (ww->deco != NULL)
    {
        zxdg_toplevel_decoration_v1_destroy (ww->deco);
        ww->deco = NULL;
    }
    if (ww->idle != NULL)
    {
        zwp_idle_inhibitor_v1_destroy (ww->idle);
        ww->idle = NULL;
    }
    if (ww->zitem != NULL)
    {
        xx_zone_item_v1_destroy (ww->zitem);
        ww->zitem = NULL;
        ww->zvalid = 0;
    }
    if (ww->subsurf != NULL)
    {
        wl_subsurface_destroy (ww->subsurf);
        ww->subsurf = NULL;
    }
    if (ww->popup != NULL)
    {
        xdg_popup_destroy (ww->popup);
        ww->popup = NULL;
    }
    if (ww->positioner != NULL)
    {
        xdg_positioner_destroy (ww->positioner);
        ww->positioner = NULL;
    }
    if (ww->toplevel != NULL)
    {
        xdg_toplevel_destroy (ww->toplevel);
        ww->toplevel = NULL;
    }
    if (ww->layer != NULL)
    {
        zwlr_layer_surface_v1_destroy (ww->layer);
        ww->layer = NULL;
    }
    if (ww->xsurf != NULL)
    {
        xdg_surface_destroy (ww->xsurf);
        ww->xsurf = NULL;
    }
    if (ww->surf != NULL)
    {
        wl_surface_destroy (ww->surf);
        ww->surf = NULL;
    }
    ww->mapped = 0;
    ww->configured = 0;
    ww->ever_presented = 0;
    ww->states = 0;
    ww->conf_w = 0;
    ww->conf_h = 0;
}

static void wl_destroy (bw_win *win)
{
    wl_win *ww = win->impl;
    int i;

    if (!win->canvas)
    {
        role_teardown (win);
        for (i = 0; i < MAX_BUFS; i++)
        {
            buf_drop (&ww->bufs[i]);
        }
        wl_display_flush (dpy);
    }
    free (ww->raster);
    free (ww);
}

static int wl_opacity (bw_win *, double alpha);
static void wl_opaque_region (bw_win *, int x, int y, int w, int h);

static void apply_surface_extras (bw_win *win)
{
    wl_win *ww = win->impl;

    if (ww->have_alpha_value)
    {
        wl_opacity (win, ww->alpha_value);
    }
    if (ww->have_opq)
    {
        wl_opaque_region (win, ww->opq_x, ww->opq_y, ww->opq_w, ww->opq_h);
    }
    /*
     * Every path fills device pixels - the raster, the whole-frame buffers and
     * the EGL window alike - so every one of them needs the scale declared, or
     * the compositor reads a device-sized buffer as that many logical pixels
     * and the window comes out the wrong size on a HiDPI output.
     */
    if (out->scale != 1)
    {
        wl_surface_set_buffer_scale (ww->surf, out->scale);
    }
}

static void wl_map (bw_win *win)
{
    wl_win *ww = win->impl;
    int i;

    if (ww->mapped || ww->kind == K_UNMAPPABLE)
    {
        return;
    }
    if (ww->surf != NULL)
    {
        return;                 /* already on its way */
    }
    ww->surf = wl_compositor_create_surface (compositor);

    switch (ww->kind)
    {
        case K_LAYER:
            ww->layer = zwlr_layer_shell_v1_get_layer_surface (layer_shell,
                ww->surf, out->wl,
                (win->flags & (BW_ABOVE | BW_POPUP))
                    ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                    : ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                "wmbench");
            zwlr_layer_surface_v1_add_listener (ww->layer, &layer_listener,
                                                win);
            zwlr_layer_surface_v1_set_size (ww->layer, (uint32_t) win->w,
                                            (uint32_t) win->h);
            zwlr_layer_surface_v1_set_anchor (ww->layer,
                ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
            zwlr_layer_surface_v1_set_margin (ww->layer,
                win->y - out->ly, 0, 0, win->x - out->lx);
            /* Panels' reserved strips would shift every coordinate the
               capture believes in */
            zwlr_layer_surface_v1_set_exclusive_zone (ww->layer, -1);
            break;

        case K_XDG_POPUP:
        {
            wl_win *pw = win->parent->impl;

            ww->xsurf = xdg_wm_base_get_xdg_surface (wm_base, ww->surf);
            xdg_surface_add_listener (ww->xsurf, &xsurf_listener, win);
            ww->positioner = xdg_wm_base_create_positioner (wm_base);
            xdg_positioner_set_size (ww->positioner, win->w, win->h);
            xdg_positioner_set_anchor_rect (ww->positioner,
                win->x - win->parent->x, win->y - win->parent->y, 1, 1);
            xdg_positioner_set_anchor (ww->positioner,
                                       XDG_POSITIONER_ANCHOR_TOP_LEFT);
            xdg_positioner_set_gravity (ww->positioner,
                                        XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
            if (pw->layer != NULL)
            {
                ww->popup = xdg_surface_get_popup (ww->xsurf, NULL,
                                                   ww->positioner);
                zwlr_layer_surface_v1_get_popup (pw->layer, ww->popup);
            }
            else
            {
                ww->popup = xdg_surface_get_popup (ww->xsurf, pw->xsurf,
                                                   ww->positioner);
            }
            xdg_popup_add_listener (ww->popup, &popup_listener, win);
            break;
        }

        case K_SUBSURFACE:
        {
            wl_win *pw = win->parent->impl;

            ww->subsurf = wl_subcompositor_get_subsurface (subcompositor,
                                                           ww->surf, pw->surf);
            wl_subsurface_set_position (ww->subsurf, win->x, win->y);
            /* Its own commits carry its own content; the parent's would
               otherwise have to drive every frame of a drag */
            wl_subsurface_set_desync (ww->subsurf);
            /* Nothing configures a subsurface, so there is nothing to wait
               for: it is on screen with the parent's next commit */
            ww->configured = 1;
            break;
        }

        case K_TOPLEVEL:
        default:
            ww->xsurf = xdg_wm_base_get_xdg_surface (wm_base, ww->surf);
            xdg_surface_add_listener (ww->xsurf, &xsurf_listener, win);
            ww->toplevel = xdg_surface_get_toplevel (ww->xsurf);
            xdg_toplevel_add_listener (ww->toplevel, &topl_listener, win);
            xdg_toplevel_set_app_id (ww->toplevel, "wmbench");
            if (win->name[0] != '\0')
            {
                xdg_toplevel_set_title (ww->toplevel, win->name);
            }
            /*
             * A frame, asked for out loud: on X11 every managed window gets
             * one from the window manager, but a Wayland toplevel is bare
             * unless the client requests server-side decoration - and a
             * bare window is less for the compositor to draw than the X11
             * run gives it. Where the protocol is missing there is no frame
             * to ask for, which is that session's own truth.
             */
            if (deco_mgr != NULL)
            {
                ww->deco = zxdg_decoration_manager_v1_get_toplevel_decoration (
                    deco_mgr, ww->toplevel);
                zxdg_toplevel_decoration_v1_set_mode (ww->deco,
                    ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            }
            if (ww->use_zone)
            {
                /* Both double-buffered: the initial commit below carries the
                   membership and the spot together */
                ww->zitem = xx_zone_manager_v1_get_zone_item (zone_mgr,
                                                              ww->toplevel);
                xx_zone_item_v1_add_listener (ww->zitem, &zitem_listener, win);
                xx_zone_v1_add_item (zone, ww->zitem);
                xx_zone_item_v1_set_position (ww->zitem,
                                              win->x - out->lx,
                                              win->y - out->ly);
            }
            if (ww->want_fullscreen)
            {
                xdg_toplevel_set_fullscreen (ww->toplevel, out->wl);
            }
            if (ww->want_maximized)
            {
                xdg_toplevel_set_maximized (ww->toplevel);
            }
            break;
    }

    /* No blanking while a benchmark window is up: a screen that sleeps
       mid-run measures the compositor drawing nothing */
    if (idle_mgr != NULL)
    {
        ww->idle = zwp_idle_inhibit_manager_v1_create_inhibitor (idle_mgr,
                                                                 ww->surf);
    }
    apply_surface_extras (win);

    /* The handshake: an empty commit, the first configure, then content */
    wl_surface_commit (ww->surf);
    for (i = 0; i < 50 && !ww->configured; i++)   /* up to five seconds */
    {
        if (!dispatch_for (100))
        {
            break;
        }
    }
    if (!ww->configured)
    {
        /* A buffer on a never-configured surface is a protocol error that
           ends the connection for every window, not just this one */
        role_teardown (win);
        wl_display_flush (dpy);

        return;
    }
    ww->mapped = 1;
    if (!ww->gl && !ww->frame_mode)
    {
        raster_ensure (win);
        wl_present_ (win);
    }
    if (ww->kind == K_SUBSURFACE)
    {
        /* A child is on screen only once its parent commits, and a parent
           that has been unmapped from under it has no surface to commit */
        wl_win *pw = win->parent->impl;

        if (pw->surf != NULL)
        {
            wl_surface_commit (pw->surf);
            wl_display_flush (dpy);
        }
    }
    if (win->name[0] != '\0')
    {
        /* The stress mix waits for this instead of asking a restacker */
        printf ("WINDOW-UP %s\n", win->name);
        fflush (stdout);
    }
}

static void wl_unmap (bw_win *win)
{
    wl_win *ww = win->impl;

    if (ww->surf == NULL)
    {
        return;
    }
    role_teardown (win);
    wl_display_flush (dpy);
}

static int wl_wait_shown (bw_win *win, int shown)
{
    wl_win *ww = win->impl;

    /* map and unmap were walked through synchronously */
    wl_display_roundtrip (dpy);

    return shown ? ww->mapped : !ww->mapped;
}

static void token_done (void *data, struct xdg_activation_token_v1 *tok,
                        const char *token)
{
    char *outbuf = data;

    snprintf (outbuf, 256, "%s", token);
    xdg_activation_token_v1_destroy (tok);
}

static const struct xdg_activation_token_v1_listener token_listener = {
    token_done
};

static int make_token (bw_win *win, char *outbuf)
{
    wl_win *ww = win->impl;
    struct xdg_activation_token_v1 *tok;
    int i;

    if (activation == NULL || ww->surf == NULL)
    {
        return 0;
    }
    outbuf[0] = '\0';
    tok = xdg_activation_v1_get_activation_token (activation);
    xdg_activation_token_v1_add_listener (tok, &token_listener, outbuf);
    xdg_activation_token_v1_set_surface (tok, ww->surf);
    xdg_activation_token_v1_set_app_id (tok, "wmbench");
    xdg_activation_token_v1_commit (tok);
    /*
     * Bounded: a compositor that means to refuse the activation may simply
     * never send 'done', and a minimise that waits for ever here is a run
     * that never reports anything at all.
     */
    for (i = 0; i < 20 && outbuf[0] == '\0'; i++)
    {
        if (!dispatch_for (100))
        {
            break;
        }
    }
    if (outbuf[0] == '\0')
    {
        xdg_activation_token_v1_destroy (tok);
    }

    return outbuf[0] != '\0';
}

static void wl_activate (bw_win *win)
{
    wl_win *ww = win->impl;
    char fresh[256];
    const char *token = NULL;

    if (activation == NULL || ww->surf == NULL || ww->kind != K_TOPLEVEL)
    {
        /* A popup or a layer surface has no focus to ask for: popups sit on
           top by construction and the overlay layer already outranks all */
        return;
    }
    if (ww->token[0] != '\0')
    {
        token = ww->token;
    }
    else if (make_token (win, fresh))
    {
        token = fresh;
    }
    if (token != NULL)
    {
        xdg_activation_v1_activate (activation, token, ww->surf);
        ww->token[0] = '\0';
        wl_display_roundtrip (dpy);
    }
}

static void wl_raise (bw_win *win)
{
    wl_activate (win);
}

static int wl_foreign_activate (const char *title);

static void wl_restore (bw_win *win)
{
    int i;

    /*
     * The way a taskbar unminimises: through the foreign-toplevel list, which
     * is the Wayland shape of the pager request the X11 side sends. It is
     * asked first because an xdg-activation token made without an input
     * serial is refused outright by some compositors, and the token was all a
     * minimised window had.
     */
    if (ftl_mgr != NULL && seat != NULL && win->name[0] != '\0')
    {
        wl_display_roundtrip (dpy);
        for (i = 0; i < nftls; i++)
        {
            if (!ftls[i].gone && ftls[i].h != NULL &&
                strcmp (ftls[i].title, win->name) == 0)
            {
                zwlr_foreign_toplevel_handle_v1_unset_minimized (ftls[i].h);
            }
        }
        wl_foreign_activate (win->name);
    }
    wl_activate (win);
}

/* Nothing here can seize the focus; the compositor alone hands it out */
static void wl_take_focus (bw_win *win)
{
    (void) win;
}

static int wl_win_placed (bw_win *win)
{
    wl_win *ww = win->impl;

    return ww->kind == K_LAYER || ww->kind == K_XDG_POPUP ||
           ww->kind == K_SUBSURFACE || ww->use_zone;
}

static int wl_win_aimable (bw_win *win)
{
    wl_win *ww = win->impl;

    /* A zone hands back positions but keeps its own origin to itself, and a
       child's coordinates are its parent's, which nothing here knows */
    return wl_win_placed (win) && !ww->use_zone && ww->kind != K_SUBSURFACE;
}

static void wl_resize_ (bw_win *win, int width, int height)
{
    wl_win *ww = win->impl;

    win->w = width;
    win->h = height;
    if (ww->layer != NULL)
    {
        zwlr_layer_surface_v1_set_size (ww->layer, (uint32_t) width,
                                        (uint32_t) height);
    }
    if (ww->mapped && !ww->gl && !ww->frame_mode)
    {
        raster_ensure (win);
        /* the new size has to reach the screen even if nothing was drawn */
        draw_damage_all (&ww->buf);
        wl_present_ (win);
    }
}

static int wl_move_raw (bw_win *win, int x, int y, int width, int height,
                        int way)
{
    wl_win *ww = win->impl;

    (void) way;
    switch (ww->kind)
    {
        case K_LAYER:
            win->x = x;
            win->y = y;
            if (ww->layer != NULL)
            {
                zwlr_layer_surface_v1_set_margin (ww->layer,
                    y - out->ly, 0, 0, x - out->lx);
                if (width > 0 && height > 0 &&
                    (width != win->w || height != win->h))
                {
                    wl_resize_ (win, width, height);
                }
                else
                {
                    wl_surface_commit (ww->surf);
                    wl_display_flush (dpy);
                }
            }
            else if (width > 0 && height > 0)
            {
                win->w = width;
                win->h = height;
            }

            return 0;

        case K_XDG_POPUP:
            /*
             * A menu, once open, is not walked around the screen here. Said
             * as a refusal and with the believed position left alone: claiming
             * the move would put coordinates nobody honoured into bw_where,
             * bench_watch and the screenshot bw_verify_at aims.
             */
            return -1;

        case K_SUBSURFACE:
        {
            wl_win *pw = win->parent->impl;

            win->x = x;
            win->y = y;
            if (ww->subsurf != NULL && pw->surf != NULL)
            {
                wl_subsurface_set_position (ww->subsurf, x, y);
                /* A position rides the parent's commit however desynchronised
                   the child is, so the parent is committed too */
                wl_surface_commit (ww->surf);
                wl_surface_commit (pw->surf);
                wl_display_flush (dpy);
            }
            if (width > 0 && height > 0 &&
                (width != win->w || height != win->h))
            {
                wl_resize_ (win, width, height);
            }

            return 0;
        }

        case K_TOPLEVEL:
            if (ww->use_zone && ww->zitem != NULL)
            {
                /* Double-buffered: the position rides the next commit */
                xx_zone_item_v1_set_position (ww->zitem,
                                              x - out->lx, y - out->ly);
                win->x = x;
                win->y = y;
                if (width > 0 && height > 0 &&
                    (width != win->w || height != win->h))
                {
                    wl_resize_ (win, width, height);
                }
                else
                {
                    wl_surface_commit (ww->surf);
                    wl_display_flush (dpy);
                }

                return 0;
            }
            /*
             * Without a zone, nothing a Wayland client says places a managed
             * toplevel. The size half of the request is the client's own, so
             * a move that carries one is honoured for that half.
             */
            if (width > 0 && height > 0)
            {
                wl_resize_ (win, width, height);

                return 0;
            }

            return -1;

        default:
            return -1;
    }
}

static void wl_where (bw_win *win, int *x, int *y, int *width, int *height)
{
    wl_win *ww = win->impl;

    /* The zone's own account outranks the belief; everyone else has only
       the belief, which bw_verify_at proves in pixels */
    if (x != NULL) *x = ww->zvalid ? out->lx + ww->zx : win->x;
    if (y != NULL) *y = ww->zvalid ? out->ly + ww->zy : win->y;
    if (width != NULL) *width = win->w;
    if (height != NULL) *height = win->h;
}

static int wl_where_live (bw_win *win)
{
    wl_win *ww = win->impl;

    return ww->use_zone && ww->zitem != NULL;
}

static void wl_maximize (bw_win *win, int on)
{
    wl_win *ww = win->impl;

    ww->want_maximized = on;
    if (ww->toplevel == NULL)
    {
        return;
    }
    if (on)
    {
        xdg_toplevel_set_maximized (ww->toplevel);
    }
    else
    {
        xdg_toplevel_unset_maximized (ww->toplevel);
    }
    wl_display_flush (dpy);
}

static void wl_minimize (bw_win *win)
{
    wl_win *ww = win->impl;

    if (ww->toplevel == NULL)
    {
        return;
    }
    /*
     * The token first, while the window is still visible and arguably has
     * standing: asked for afterwards, focus-stealing prevention can refuse
     * to unminimise, which is risk 1 of the plan and exit 3 of the run.
     */
    make_token (win, ww->token);
    xdg_toplevel_set_minimized (ww->toplevel);
    wl_display_flush (dpy);
}

static void wl_fullscreen_ (bw_win *win, int on)
{
    wl_win *ww = win->impl;

    ww->want_fullscreen = on;
    if (ww->toplevel == NULL)
    {
        return;                 /* remembered for the map */
    }
    if (on)
    {
        xdg_toplevel_set_fullscreen (ww->toplevel, out->wl);
    }
    else
    {
        xdg_toplevel_unset_fullscreen (ww->toplevel);
    }
    wl_display_flush (dpy);
}

static unsigned wl_state (bw_win *win)
{
    wl_win *ww = win->impl;

    wl_display_roundtrip (dpy);

    return ww->states;
}

static int wl_opacity (bw_win *win, double alpha)
{
    wl_win *ww = win->impl;

    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    ww->alpha_value = alpha;
    ww->have_alpha_value = 1;
    if (alpha_mod == NULL)
    {
        return 0;
    }
    if (ww->surf != NULL)
    {
        if (ww->alpha == NULL)
        {
            ww->alpha = wp_alpha_modifier_v1_get_surface (alpha_mod, ww->surf);
        }
        wp_alpha_modifier_surface_v1_set_multiplier (ww->alpha,
            (uint32_t) (alpha * 4294967295.0));
        wl_surface_commit (ww->surf);
        wl_display_flush (dpy);
    }

    return 1;
}

static void wl_opaque_region (bw_win *win, int x, int y, int width, int height)
{
    wl_win *ww = win->impl;

    ww->opq_x = x;
    ww->opq_y = y;
    ww->opq_w = width;
    ww->opq_h = height;
    ww->have_opq = 1;
    if (ww->surf != NULL)
    {
        struct wl_region *reg = wl_compositor_create_region (compositor);

        wl_region_add (reg, x, y, width, height);
        wl_surface_set_opaque_region (ww->surf, reg);
        /* The compositor took its own copy; kept, it would leak one object
           per map, and apply_surface_extras replays this on every one */
        wl_region_destroy (reg);
        wl_surface_commit (ww->surf);
    }
}

static void wl_background_colour (bw_win *win, unsigned long colour)
{
    ((wl_win *) win->impl)->bg_colour = colour;
}

static void wl_set_background (bw_win *win, bw_win *canvas)
{
    ((wl_win *) win->impl)->bg_canvas = canvas;
}

static draw_buf *target_of (bw_win *win)
{
    wl_win *ww = win->impl;

    if (!win->canvas)
    {
        raster_ensure (win);
    }

    return &ww->buf;
}

static int S (int v)
{
    return v * out->scale;
}

static void wl_fill (bw_win *win, unsigned long c, int x, int y,
                     int width, int height)
{
    draw_buf *b = target_of (win);

    if (b->px != NULL)
    {
        draw_fill (b, c, S (x), S (y), S (width), S (height));
    }
}

static void wl_rect (bw_win *win, unsigned long c, int x, int y,
                     int width, int height)
{
    draw_buf *b = target_of (win);

    if (b->px != NULL)
    {
        draw_rect (b, c, S (x), S (y), S (width), S (height), out->scale);
    }
}

static void wl_poly (bw_win *win, unsigned long c, const bw_point *p, int n)
{
    draw_buf *b = target_of (win);
    bw_point sp[16];
    int i;

    if (b->px == NULL || n > 16)
    {
        return;
    }
    for (i = 0; i < n; i++)
    {
        sp[i].x = S (p[i].x);
        sp[i].y = S (p[i].y);
    }
    draw_poly (b, c, sp, n);
}

static void wl_text (bw_win *win, unsigned long c, int x, int y, const char *s)
{
    draw_buf *b = target_of (win);

    if (b->px != NULL)
    {
        draw_text (b, c, S (x), S (y), s, out->scale);
    }
}

static void wl_clip (bw_win *win, int x, int y, int width, int height)
{
    draw_buf *b = target_of (win);

    if (width < 0)
    {
        draw_clip (b, 0, 0, -1, -1);
    }
    else
    {
        draw_clip (b, S (x), S (y), S (width), S (height));
    }
}

static void wl_copy (bw_win *src, bw_win *dst, int sx, int sy,
                     int width, int height, int dx, int dy)
{
    draw_buf *db = target_of (dst);
    draw_buf *sb = target_of (src);

    if (db->px == NULL || sb->px == NULL)
    {
        return;
    }
    /* Both sides hold buffer pixels, so one scaling of the caller's logical
       coordinates covers the copy */
    draw_copy (db, sb, S (sx), S (sy), S (width), S (height), S (dx), S (dy));
}

static void *wl_frame_pixels (bw_win *win, int *stride)
{
    wl_win *ww = win->impl;
    wl_buf *b;

    if (ww->surf == NULL)
    {
        return NULL;
    }
    ww->frame_mode = 1;
    /*
     * Device pixels, matching the buffer scale the surface carries: a
     * logical-sized buffer under a scale of 2 would put the window on screen
     * at half the size the X11 run gives it.
     */
    b = buf_get (ww, win->w * out->scale, win->h * out->scale);
    if (b == NULL)
    {
        return NULL;
    }
    ww->frame_out = b;
    *stride = b->w * 4;

    return b->px;
}

static void wl_frame_size (bw_win *win, int *width, int *height)
{
    if (width != NULL) *width = win->w * out->scale;
    if (height != NULL) *height = win->h * out->scale;
}

static void wl_frame_push (bw_win *win)
{
    wl_win *ww = win->impl;

    if (ww->frame_out == NULL || ww->surf == NULL)
    {
        return;
    }
    wl_surface_attach (ww->surf, ww->frame_out->wb, 0, 0);
    wl_surface_damage_buffer (ww->surf, 0, 0,
                              ww->frame_out->w, ww->frame_out->h);
    wl_surface_commit (ww->surf);
    wl_display_flush (dpy);
    ww->frame_out->busy = 1;
    ww->frame_out = NULL;
}

static bw_image *wl_capture (int x, int y, int w, int h)
{
    if (fractional ())
    {
        return NULL;
    }

    return capture_via_cmd (x - out->lx, y - out->ly, w, h, out->scale,
                            out->mode_w, out->mode_h);
}

static int wl_verify_at (bw_win *win)
{
    wl_win *ww = win->impl;
    bw_image *img;
    int gx, gy, gw, gh, sx, sy, looked = 0, hit = 0;

    if (ww->use_zone && ww->zitem != NULL)
    {
        /* The compositor reports the position itself; there is no belief
           left for a screenshot to prove */
        return 1;
    }
    if (ww->raster == NULL || !ww->mapped)
    {
        return -1;
    }
    gx = win->x;
    gy = win->y;
    gw = win->w;
    gh = win->h;
    /* Only what is on the screen can be photographed */
    if (gx < out->lx) { gw -= out->lx - gx; gx = out->lx; }
    if (gy < out->ly) { gh -= out->ly - gy; gy = out->ly; }
    if (gx + gw > out->lx + out->lw) gw = out->lx + out->lw - gx;
    if (gy + gh > out->ly + out->lh) gh = out->ly + out->lh - gy;
    if (gw < 16 || gh < 16)
    {
        return -1;
    }
    img = wl_capture (gx, gy, gw, gh);
    if (img == NULL)
    {
        return -1;
    }
    for (sy = 8; sy < gh - 8; sy += (gh - 16) / 4 + 1)
    {
        for (sx = 8; sx < gw - 8; sx += (gw - 16) / 4 + 1)
        {
            /* the same spot in our raster, in buffer pixels */
            int rx = (gx - win->x + sx) * out->scale;
            int ry = (gy - win->y + sy) * out->scale;
            unsigned long want, got;
            int dr, dg, db;

            if (rx >= ww->bw || ry >= ww->bh)
            {
                continue;
            }
            want = ww->raster[(size_t) ry * ww->buf.stride + rx] & 0xffffff;
            got = bw_pixel (img, sx, sy);
            dr = (int) ((want >> 16) & 0xff) - (int) ((got >> 16) & 0xff);
            dg = (int) ((want >> 8) & 0xff) - (int) ((got >> 8) & 0xff);
            db = (int) (want & 0xff) - (int) (got & 0xff);
            looked++;
            /* Colour pipelines shave a bit; a wrong window is off by miles */
            if (dr * dr + dg * dg + db * db < 24 * 24)
            {
                hit++;
            }
        }
    }
    bw_image_free (img);
    if (looked == 0)
    {
        return -1;
    }

    return hit * 10 >= looked * 9;
}

static void *wl_native_display (void)
{
    return dpy;
}

static void *wl_native_surface (bw_win *win)
{
    return ((wl_win *) win->impl)->surf;
}

static int wl_foreign_available (void)
{
    return ftl_mgr != NULL;
}

static int wl_foreign_exists (const char *title)
{
    int i;

    wl_display_roundtrip (dpy);
    for (i = 0; i < nftls; i++)
    {
        if (!ftls[i].gone && ftls[i].h != NULL &&
            strcmp (ftls[i].title, title) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int wl_foreign_activate (const char *title)
{
    int i, n = 0;

    /* activate takes a seat; a null one is a protocol error and the end of
       the connection, so a session without one simply cannot be asked */
    if (ftl_mgr == NULL || seat == NULL)
    {
        return 0;
    }
    wl_display_roundtrip (dpy);
    for (i = 0; i < nftls; i++)
    {
        if (!ftls[i].gone && ftls[i].h != NULL &&
            strcmp (ftls[i].title, title) == 0)
        {
            zwlr_foreign_toplevel_handle_v1_activate (ftls[i].h, seat);
            n++;
        }
    }
    wl_display_roundtrip (dpy);

    return n;
}

const struct bw_ops bw_wl_ops = {
    wl_open, wl_close, wl_screen_size, wl_stage,
    wl_create, wl_destroy, wl_map, wl_unmap, wl_wait_shown,
    wl_raise, wl_activate, wl_restore, wl_take_focus,
    wl_win_placed, wl_win_aimable, wl_move_raw, wl_where, wl_where_live, wl_resize_,
    wl_maximize, wl_minimize, wl_fullscreen_, wl_state,
    wl_opacity, wl_opaque_region, wl_background_colour, wl_set_background,
    wl_canvas_new,
    wl_fill, wl_rect, wl_poly, wl_text, wl_clip, wl_copy,
    wl_frame_pixels, wl_frame_size, wl_frame_push,
    wl_present_, wl_sync_, wl_pump_,
    wl_capture, wl_verify_at,
    wl_native_display, wl_native_surface,
    wl_foreign_available, wl_foreign_exists, wl_foreign_activate,
};
