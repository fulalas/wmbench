/*
 * Monotonic, so nothing here can be moved by the system clock being set, and
 * one clock for the whole suite, so everything means the same by a second.
 */
#ifndef BENCH_NOW_H
#define BENCH_NOW_H

double bench_now (void);

#endif
