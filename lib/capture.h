/*
 * One way to photograph the screen for every check.
 *
 * On plain X11 it is XGetImage on the root, same as always. Under a Wayland
 * compositor that only shows XWayland's idea of the screen, not the real one,
 * so when BENCH_CAPTURE_CMD is set it runs that command instead; the command
 * gets one argument, a path, and must write a full-screen binary PPM (P6)
 * there. The region is then cut out of that image.
 *
 * The result behaves like any XImage: XGetPixel and XDestroyImage work.
 */
#ifndef BENCH_CAPTURE_H
#define BENCH_CAPTURE_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>

XImage *capture_region (Display *d, Window root,
                        int x, int y, unsigned int w, unsigned int h);

#endif
