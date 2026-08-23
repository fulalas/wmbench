/*
 * The patch of screen the benchmark is allowed to use.
 *
 * It is 1920x1080, or the whole screen where the screen is smaller, centred.
 * Nothing the benchmark puts on the screen goes outside it - what a window
 * manager does with a window of its own, maximised, snapped to half the
 * screen or fullscreen, is that window manager's business and uses the real
 * screen.
 *
 * Two reasons for a fixed size rather than a share of the screen. It has to
 * run on a 1080p display without windows hanging off the edge, since a window
 * manager that clamps a frame to the screen then measures something different
 * from one that lets it hang off. And the work has to be the same everywhere:
 * sized by the screen, a 4K desktop would composite four times the pixels of
 * a 1080p one and the two numbers could not be put side by side.
 */
#include <X11/Xlib.h>
#include "stage.h"

#define STAGE_W 1920
#define STAGE_H 1080

void bench_stage (Display *d, int margin, int *x, int *y, int *w, int *h)
{
    int scr = DefaultScreen (d);
    int screen_w = DisplayWidth (d, scr);
    int screen_h = DisplayHeight (d, scr);
    int stage_w = (screen_w < STAGE_W) ? screen_w : STAGE_W;
    int stage_h = (screen_h < STAGE_H) ? screen_h : STAGE_H;

    *x = (screen_w - stage_w) / 2 + margin;
    *y = (screen_h - stage_h) / 2 + margin;
    *w = stage_w - 2 * margin;
    *h = stage_h - 2 * margin;
    if (*w < 1)
    {
        *w = 1;
    }
    if (*h < 1)
    {
        *h = 1;
    }
}

void bench_fit (Display *d, int *w, int *h, int margin)
{
    int x, y, room_w, room_h;

    bench_stage (d, margin, &x, &y, &room_w, &room_h);
    if (*w > room_w)
    {
        *w = room_w;
    }
    if (*h > room_h)
    {
        *h = room_h;
    }
}
