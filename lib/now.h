/*
 * One clock for the whole suite: seconds since some fixed point in the past,
 * monotonic, so nothing here can be moved by the system clock being set.
 *
 * Every benchmark and check paces itself and reports its own rate with this,
 * and they all have to mean the same thing by a second.
 */
#ifndef BENCH_NOW_H
#define BENCH_NOW_H

double bench_now (void);

#endif
