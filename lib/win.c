#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win_priv.h"

static const struct bw_ops *ops;

int bw_open (void)
{
    const char *want = getenv ("BENCH_BACKEND");
    const char *wl = getenv ("WAYLAND_DISPLAY");

    /*
     * BENCH_BACKEND=x11|wayland overrides the session, which is how a native
     * run and an XWayland run of the same compositor are put side by side.
     * Left alone, a live Wayland socket is the fact of the matter.
     */
    if (want != NULL && strcmp (want, "x11") == 0)
    {
        ops = &bw_x11_ops;
    }
    else if (want != NULL && strcmp (want, "wayland") == 0)
    {
        ops = &bw_wl_ops;
    }
    else
    {
        ops = (wl != NULL && wl[0] != '\0') ? &bw_wl_ops : &bw_x11_ops;
    }
    if (!ops->open ())
    {
        ops = NULL;

        return 0;
    }

    return 1;
}

void bw_close (void)
{
    if (ops != NULL)
    {
        ops->close ();
    }
    ops = NULL;
}

int bw_is_wayland (void)
{
    return ops == &bw_wl_ops;
}

void bw_screen_size (int *w, int *h) { ops->screen_size (w, h); }

void bw_stage (int margin, int *x, int *y, int *w, int *h)
{
    ops->stage (margin, x, y, w, h);
}

bw_win *bw_create (bw_win *parent, int x, int y, int w, int h,
                   const char *name, unsigned flags)
{
    bw_win *win = calloc (1, sizeof *win);

    if (win == NULL)
    {
        return NULL;
    }
    win->flags = flags;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->parent = parent;
    if (name != NULL)
    {
        snprintf (win->name, sizeof win->name, "%s", name);
    }
    if (!ops->create (win))
    {
        free (win);

        return NULL;
    }

    return win;
}

bw_win *bw_canvas (int w, int h)
{
    bw_win *win = calloc (1, sizeof *win);

    if (win == NULL)
    {
        return NULL;
    }
    win->canvas = 1;
    win->w = w;
    win->h = h;
    if (!ops->canvas_new (win))
    {
        free (win);

        return NULL;
    }

    return win;
}

void bw_destroy (bw_win *win)
{
    if (win == NULL)
    {
        return;
    }
    ops->destroy (win);
    free (win);
}

void bw_map (bw_win *w)                   { ops->map (w); }
void bw_unmap (bw_win *w)                 { ops->unmap (w); }
int  bw_wait_shown (bw_win *w, int shown) { return ops->wait_shown (w, shown); }
void bw_raise (bw_win *w)                 { ops->raise (w); }
void bw_activate (bw_win *w)              { ops->activate (w); }
void bw_restore (bw_win *w)               { ops->restore (w); }
void bw_take_focus (bw_win *w)            { ops->take_focus (w); }

int  bw_win_placed (bw_win *w)            { return ops->win_placed (w); }
int  bw_win_aimable (bw_win *w)           { return ops->win_aimable (w); }

int bw_move_raw (bw_win *w, int x, int y, int width, int height, int way)
{
    return ops->move_raw (w, x, y, width, height, way);
}

void bw_where (bw_win *w, int *x, int *y, int *width, int *height)
{
    ops->where (w, x, y, width, height);
}

int bw_where_live (bw_win *w)
{
    return ops->where_live (w);
}

void bw_resize (bw_win *w, int width, int height)
{
    ops->resize (w, width, height);
}

void bw_maximize (bw_win *w, int on)      { ops->maximize (w, on); }
void bw_minimize (bw_win *w)              { ops->minimize (w); }
void bw_fullscreen (bw_win *w, int on)    { ops->fullscreen (w, on); }
unsigned bw_state (bw_win *w)             { return ops->state (w); }
int  bw_opacity (bw_win *w, double a)     { return ops->opacity (w, a); }

void bw_opaque_region (bw_win *w, int x, int y, int width, int height)
{
    ops->opaque_region (w, x, y, width, height);
}

void bw_background_colour (bw_win *w, unsigned long c)
{
    ops->background_colour (w, c);
}

void bw_set_background (bw_win *w, bw_win *canvas)
{
    ops->set_background (w, canvas);
}

void bw_fill (bw_win *w, unsigned long c, int x, int y, int width, int height)
{
    ops->fill (w, c, x, y, width, height);
}

void bw_rect (bw_win *w, unsigned long c, int x, int y, int width, int height)
{
    ops->rect (w, c, x, y, width, height);
}

void bw_poly (bw_win *w, unsigned long c, const bw_point *p, int n)
{
    ops->poly (w, c, p, n);
}

void bw_text (bw_win *w, unsigned long c, int x, int y, const char *s)
{
    ops->text (w, c, x, y, s);
}

void bw_clip (bw_win *w, int x, int y, int width, int height)
{
    ops->clip (w, x, y, width, height);
}

void bw_copy (bw_win *src, bw_win *dst, int sx, int sy, int w, int h,
              int dx, int dy)
{
    ops->copy (src, dst, sx, sy, w, h, dx, dy);
}

void *bw_frame_pixels (bw_win *w, int *stride)
{
    return ops->frame_pixels (w, stride);
}

void bw_frame_size (bw_win *w, int *width, int *height)
{
    ops->frame_size (w, width, height);
}

void bw_frame_push (bw_win *w)            { ops->frame_push (w); }

void bw_present (bw_win *w)               { ops->present (w); }
void bw_sync (void)                       { ops->sync (); }
void bw_pump (void)                       { ops->pump (); }

bw_image *bw_capture (int x, int y, int w, int h)
{
    return ops->capture (x, y, w, h);
}

int bw_verify_at (bw_win *w)              { return ops->verify_at (w); }

void *bw_native_display (void)            { return ops->native_display (); }
void *bw_native_surface (bw_win *w)       { return ops->native_surface (w); }

int bw_foreign_available (void)           { return ops->foreign_available (); }
int bw_foreign_exists (const char *t)     { return ops->foreign_exists (t); }
int bw_foreign_activate (const char *t)   { return ops->foreign_activate (t); }
