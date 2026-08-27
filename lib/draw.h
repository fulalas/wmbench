/* Coordinates are buffer pixels: the caller has applied the output scale */
#ifndef BENCH_DRAW_H
#define BENCH_DRAW_H

#include "win.h"

typedef struct {
    unsigned int *px;
    int w, h;
    int stride;                 /* pixels a line really occupies; >= w, so a
                                   resize can keep the pixels where they are */
    int argb;                   /* keep alpha as given, premultiplied */
    int clip_on;
    int cx, cy, cw, ch;
    int dx0, dy0, dx1, dy1;     /* damage, exclusive on the high side */
} draw_buf;

void draw_init (draw_buf *, unsigned int *px, int w, int h, int stride,
                int argb);
void draw_damage_reset (draw_buf *);
int  draw_damaged (const draw_buf *, int *x, int *y, int *w, int *h);

void draw_fill (draw_buf *, unsigned long colour, int x, int y, int w, int h);
void draw_rect (draw_buf *, unsigned long colour, int x, int y, int w, int h,
                int thick);
void draw_poly (draw_buf *, unsigned long colour, const bw_point *p, int n);
/* From the baseline, like X core text; scale grows the 6x9 cell whole */
void draw_text (draw_buf *, unsigned long colour, int x, int y, const char *s,
                int scale);
void draw_clip (draw_buf *, int x, int y, int w, int h);   /* w < 0 clears */
void draw_note (draw_buf *, int x0, int y0, int x1, int y1);  /* add damage */
void draw_damage_all (draw_buf *);
void draw_copy (draw_buf *dst, const draw_buf *src, int sx, int sy,
                int w, int h, int dx, int dy);

#endif
