#ifndef BENCH_PLACE_H
#define BENCH_PLACE_H
#include "win.h"

/*
 * The stress mix stacks its wide backgrounds on one another. They share this
 * margin, and each one above the bottom stops STAGE_STRIP further in, so every
 * one of them keeps a strip of its own on screen. A window covered whole is
 * not composited at all on most compositors, and it cannot be found in a
 * screenshot either.
 *
 * Every load that shares the frame asks bw_stage for STAGE_MARGIN, fsbench
 * included: it is the window under the others, and the strips are counted in
 * from its edge. One of them keeping a margin of its own would move that edge
 * and no compiler would say so.
 *
 * A stage can be narrower than the steps need - bw_stage promises a width of
 * 1 and nothing more - so a width worked out from them is floored rather than
 * handed to bw_create as it comes.
 */
#define STAGE_MARGIN 60
#define STAGE_STRIP  50
#define STAGE_MINW   80

/*
 * Say where a window was put against where it was asked to go. Prints one
 * "place:" line, and "PLACE-IGNORED" as well when it is nowhere near.
 * Returns 1 when the position was honoured.
 *
 * Where the session only believes the position - layer-shell margins, which
 * bw_where reports back unchanged - one screenshot proves it, the way the
 * move probe does. See bench_prove_places.
 */
int bench_placed (bw_win *, int ax, int ay, const char *what);

/* Managed windows only: nothing resizes an unmanaged one */
int bench_sized (bw_win *, const char *what);

/* That proof costs about 0.3 s on Wayland, so it must not be taken between
   MEASURE-START and MEASURE-END. Turn it off around a measured phase; the
   "place:" line then says the position is the protocol's word. */
void bench_prove_places (int on);

/*
 * Which way of moving a window this session honours. On X11 it tries the
 * plain client call and then the pager request; on Wayland it asks the
 * backend and proves the answer with one screenshot. Prints one "moves:"
 * line, and "MOVE-REFUSED" when nothing works.
 *
 * Returns 0 for the pager request, 1 for plain calls (or the Wayland way),
 * -1 for neither.
 */
int bench_probe_move (bw_win *, int x, int y, int w, int h);

/* Move a window the way bench_probe_move found works. Size 0 means leave it */
void bench_move (bw_win *, int x, int y, int w, int h);

/* Hold until bench_now() reaches due, keeping the session's connection fed */
void bench_wait_until (double due);

/* Can a capture be aimed at this window? Prints why not and returns 0. A
   zone-placed window is placed and still unaimable: its origin is opaque. */
int bench_aimable (bw_win *);

/* usagebench flips the way mid-run when a granted position hides a refused
   size; the flip belongs with the way, which lives here */
void bench_flip_way (void);

/*
 * Did the window ever actually go anywhere? bench_watch() takes a sample,
 * cheaply; bench_moved() answers at the end of the run. A probe can only say
 * what happened once, and at least one window manager answers it differently
 * from one run to the next, so the run itself has the last word.
 */
void bench_watch (bw_win *);
int  bench_moved (void);

/* Before a watched window is destroyed. The box is kept by pointer, and the
   allocator hands the same address out again: without this the window opened
   next goes on widening the box the old one left */
void bench_unwatch (bw_win *);

/* On Wayland, the second half of the proof: one screenshot at the believed
   spot after MEASURE-END, before the window goes away. X11 needs none. */
void bench_verify_end (bw_win *);

#endif
