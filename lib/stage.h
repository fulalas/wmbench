#ifndef BENCH_STAGE_H
#define BENCH_STAGE_H

#include <X11/Xlib.h>

/*
 * The area the benchmark may put windows in: 1920x1080 or the screen if it is
 * smaller, centred, less the margin asked for. See stage.c.
 */
void bench_stage (Display *d, int margin, int *x, int *y, int *w, int *h);

/* Shrink a wanted window size to that area, leaving it alone if it fits */
void bench_fit (Display *d, int *w, int *h, int margin);

#endif
