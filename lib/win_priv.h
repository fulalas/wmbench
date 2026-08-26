#ifndef BENCH_WIN_PRIV_H
#define BENCH_WIN_PRIV_H

#include "win.h"
#include "capture.h"

struct bw_win {
    unsigned flags;
    int canvas;                 /* an off-screen draw target, never mapped */
    char name[64];
    /* believed geometry: what was asked for, then what the session said */
    int x, y, w, h;
    bw_win *parent;
    void *impl;                 /* the backend's own state */
};

struct bw_ops {
    int  (*open) (void);
    void (*close) (void);
    void (*screen_size) (int *w, int *h);
    void (*stage) (int margin, int *x, int *y, int *w, int *h);

    int  (*create) (bw_win *);          /* fills impl; 0 on failure */
    void (*destroy) (bw_win *);
    void (*map) (bw_win *);
    void (*unmap) (bw_win *);
    int  (*wait_shown) (bw_win *, int shown);
    void (*raise) (bw_win *);
    void (*activate) (bw_win *);
    void (*restore) (bw_win *);
    void (*take_focus) (bw_win *);

    int  (*win_placed) (bw_win *);
    int  (*win_aimable) (bw_win *);
    int  (*move_raw) (bw_win *, int x, int y, int w, int h, int way);
    void (*where) (bw_win *, int *x, int *y, int *w, int *h);
    int  (*where_live) (bw_win *);
    void (*resize) (bw_win *, int w, int h);

    void (*maximize) (bw_win *, int on);
    void (*minimize) (bw_win *);
    void (*fullscreen) (bw_win *, int on);
    unsigned (*state) (bw_win *);
    int  (*opacity) (bw_win *, double alpha);
    void (*opaque_region) (bw_win *, int x, int y, int w, int h);
    void (*background_colour) (bw_win *, unsigned long colour);
    void (*set_background) (bw_win *, bw_win *canvas);

    int  (*canvas_new) (bw_win *);

    void (*fill) (bw_win *, unsigned long c, int x, int y, int w, int h);
    void (*rect) (bw_win *, unsigned long c, int x, int y, int w, int h);
    void (*poly) (bw_win *, unsigned long c, const bw_point *p, int n);
    void (*text) (bw_win *, unsigned long c, int x, int y, const char *s);
    void (*clip) (bw_win *, int x, int y, int w, int h);
    void (*copy) (bw_win *src, bw_win *dst, int sx, int sy, int w, int h,
                  int dx, int dy);

    void *(*frame_pixels) (bw_win *, int *stride);
    void (*frame_size) (bw_win *, int *w, int *h);
    void (*frame_push) (bw_win *);

    void (*present) (bw_win *);
    void (*sync) (void);
    void (*pump) (void);

    bw_image *(*capture) (int x, int y, int w, int h);
    int  (*verify_at) (bw_win *);

    void *(*native_display) (void);
    void *(*native_surface) (bw_win *);

    int  (*foreign_available) (void);
    int  (*foreign_exists) (const char *title);
    int  (*foreign_activate) (const char *title);
};

extern const struct bw_ops bw_x11_ops;
extern const struct bw_ops bw_wl_ops;

#endif
