#ifndef BENCH_WIN_H
#define BENCH_WIN_H

#include "capture.h"

typedef struct bw_win bw_win;

typedef struct { int x, y; } bw_point;

#define BW_PLACED  1u
#define BW_POPUP   2u
#define BW_ABOVE   4u
#define BW_ARGB    8u
#define BW_FIXED  16u
#define BW_NOTIFY 32u
#define BW_GL     64u
/* Placed on Wayland, but no size hints on X11: the tools that never set them
   keep measuring what they measured */
#define BW_LOOSE 128u
/* A resize keeps the pixels already drawn. Without it the whole window
   refills from the background, which is X11's default and part of what the
   resizing loads measure */
#define BW_KEEP  256u
/* A capture will be aimed at the believed position, so it must be a screen
   coordinate: zone placement, whose origin is deliberately opaque, does not
   count */
#define BW_AIMED 512u
/* Walks manager states, which only a managed toplevel has - never a layer
   surface, however placeable those are */
#define BW_STATED 1024u
/* The program places this window itself and no manager frames it: override
   redirect on X11, a layer surface on Wayland. Wayland cannot both decorate
   and place a window, so the tests that need their own coordinates go
   unmanaged on both sides rather than measuring two different scenes */
#define BW_UNMANAGED 2048u
/* Put where the parent is rather than where the screen is: x and y are the
   parent's coordinates, so nothing has to know where on screen the parent
   went, which is the only way a session that places nothing can be asked for
   a position at all. A subsurface on Wayland; on X11 a window of its own at
   the parent's corner plus the offset, because a real child window is cleared
   to its background wherever a moving sibling leaves it */
#define BW_CHILD 4096u

#define BW_STATE_MAX         1u
#define BW_STATE_FULLSCREEN  2u
#define BW_STATE_ACTIVE      4u

int  bw_open (void);
void bw_close (void);
int  bw_is_wayland (void);
void bw_screen_size (int *w, int *h);
/* 1920x1080 or the primary monitor if it is smaller, centred on it, less the
   margin asked for */
void bw_stage (int margin, int *x, int *y, int *w, int *h);

/* parent matters only to a popup, and only on Wayland, where a menu belongs
   to the window it opens over */
bw_win *bw_create (bw_win *parent, int x, int y, int w, int h,
                   const char *name, unsigned flags);
void bw_destroy (bw_win *);
void bw_map (bw_win *);
void bw_unmap (bw_win *);
int  bw_wait_shown (bw_win *, int shown);   /* needs BW_NOTIFY */
void bw_raise (bw_win *);
void bw_activate (bw_win *);
void bw_restore (bw_win *);
/* Take the input focus outright, over and above asking politely. X11 only:
   on Wayland the compositor alone decides, so this does nothing there */
void bw_take_focus (bw_win *);

/* Whether this session puts this window where this program says. A managed
   Wayland toplevel without zones: no, and bench_placed() turns that into
   PLACE-IGNORED */
int  bw_win_placed (bw_win *);
/* Is the believed position a screen coordinate a capture can be aimed at?
   A zone-placed window is placed and still unaimable: its origin is opaque */
int  bw_win_aimable (bw_win *);
/* 0 done, -1 the session cannot move this window at all. way is X11's two
   ways of asking (1 plain, 0 the pager request); Wayland ignores it */
int  bw_move_raw (bw_win *, int x, int y, int w, int h, int way);
void bw_where (bw_win *, int *x, int *y, int *w, int *h);
void bw_asked_size (bw_win *, int *w, int *h);
/* Does bw_where() carry the session's own answer rather than a belief?
   X11 and zone placement: yes. Layer-shell margins: no, hence the proof */
int  bw_where_live (bw_win *);
void bw_resize (bw_win *, int w, int h);

void bw_maximize (bw_win *, int on);
void bw_minimize (bw_win *);
void bw_fullscreen (bw_win *, int on);
unsigned bw_state (bw_win *);
int  bw_opacity (bw_win *, double alpha);   /* 0 when the session cannot */
void bw_opaque_region (bw_win *, int x, int y, int w, int h);
void bw_background_colour (bw_win *, unsigned long colour);
void bw_set_background (bw_win *, bw_win *canvas);

bw_win *bw_canvas (int w, int h);

/* Colours are 0xRRGGBB, or 0xAARRGGBB into a BW_ARGB window. bw_text draws
   from the baseline, the way X core text does */
void bw_fill (bw_win *, unsigned long colour, int x, int y, int w, int h);
void bw_rect (bw_win *, unsigned long colour, int x, int y, int w, int h);
void bw_poly (bw_win *, unsigned long colour, const bw_point *p, int n);
void bw_text (bw_win *, unsigned long colour, int x, int y, const char *s);
void bw_clip (bw_win *, int x, int y, int w, int h);   /* w < 0 clears it */
void bw_copy (bw_win *src, bw_win *dst, int sx, int sy, int w, int h,
              int dx, int dy);

void *bw_frame_pixels (bw_win *, int *stride);
/* The pixels a frame really holds, which is the window scaled up on a HiDPI
   output: the caller fills that, not what it asked for */
void bw_frame_size (bw_win *, int *w, int *h);
void bw_frame_push (bw_win *);

void bw_present (bw_win *);
void bw_sync (void);
void bw_pump (void);            /* keep the connection fed inside a sleep loop */

/* NULL is could-not-look, which is not the same as a wrong pixel */
bw_image *bw_capture (int x, int y, int w, int h);

/* Photograph own pixels at the believed spot. 1 they are there, 0 they are
   not, -1 the screen cannot be looked at. Costs a screenshot on Wayland:
   never call it inside a measured window */
int  bw_verify_at (bw_win *);

void *bw_native_display (void);
void *bw_native_surface (bw_win *);

int bw_foreign_available (void);
int bw_foreign_exists (const char *title);
int bw_foreign_activate (const char *title);

#endif
