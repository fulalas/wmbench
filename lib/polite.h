/*
 * Raising or focusing a window without a real user timestamp reads as focus
 * stealing, and GNOME answers with a "window is ready" notification instead
 * of doing it. This asks the way a pager does, with the server's own time.
 */
#ifndef BENCH_POLITE_H
#define BENCH_POLITE_H

#include <X11/Xlib.h>

void polite_activate (Display *d, Window root, Window w);

#endif
