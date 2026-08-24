#ifndef BENCH_PLACE_H
#define BENCH_PLACE_H
#include <X11/Xlib.h>

/* Where a window really is, frame and all, in root coordinates */
void bench_where (Display *d, Window w, int *x, int *y, int *width, int *height);

/*
 * Say where a window was put against where it was asked to go. Prints one
 * "place:" line, and "PLACE-IGNORED" as well when it is nowhere near.
 * Returns 1 when the position was honoured.
 */
int bench_placed (Display *d, Window w, int ax, int ay, const char *what);

/*
 * Which way of moving a window this session honours. Asks the pager way
 * (_NET_MOVERESIZE_WINDOW) for a place the window is not already in, looks,
 * then tries a plain client call. Prints one "moves:" line, and
 * "MOVE-REFUSED" when neither works.
 *
 * Returns 0 for the pager request, 1 for plain calls, -1 for neither.
 */
int bench_probe_move (Display *d, Window root, Window w,
                      int x, int y, int width, int height);

/*
 * Did the window ever actually go anywhere? bench_watch() takes a sample,
 * cheaply; bench_moved() answers at the end of the run. A probe can only say
 * what happened once, and at least one window manager answers it differently
 * from one run to the next, so the run itself has the last word.
 */
void bench_watch (Display *d, Window w);
int bench_moved (void);

/* Move a window the way bench_probe_move found works. Size 0 means leave it */
void bench_move (Display *d, Window root, Window w,
                 int x, int y, int width, int height, int plain);

#endif
