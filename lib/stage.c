/*
 * The patch of screen the benchmark is allowed to use.
 *
 * It is 1920x1080, or the whole monitor where the monitor is smaller, centred
 * on the primary monitor.
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
#include <dlfcn.h>
#include <X11/Xlib.h>
#include "stage.h"

#define STAGE_W 1920
#define STAGE_H 1080

/* Xinerama's answer for one output, laid out as it has been since 1998 */
typedef struct {
    int screen_number;
    short x_org, y_org, width, height;
} output_info;

/*
 * Where the primary monitor is.
 *
 * DisplayWidth and DisplayHeight give the union of every output, so on two
 * 1920x1080 monitors side by side the middle of the screen is the seam
 * between them: a stage centred there is split down the middle, composited
 * and presented twice, and the number cannot be put beside a single-monitor
 * one. Xinerama knows where each output really is, and the server lists the
 * primary one first.
 *
 * It is opened by hand rather than linked so that every program drawing a
 * stage does not gain a library it needs only here, and so that a desktop
 * without Xinerama simply keeps the whole-screen answer. The handle is never
 * closed: the library leaves a hook behind in the Display that XCloseDisplay
 * would call after it had gone.
 */
static int primary_monitor (Display *d, int *x, int *y, int *w, int *h)
{
    static output_info *(*query) (Display *, int *);
    static int looked;
    output_info *out;
    int n = 0, ok = 0;

    if (!looked)
    {
        void *lib = dlopen ("libXinerama.so.1", RTLD_LAZY);

        looked = 1;
        if (lib != NULL)
        {
            query = (output_info *(*) (Display *, int *))
                    dlsym (lib, "XineramaQueryScreens");
        }
    }
    if (query == NULL)
    {
        return 0;
    }
    out = query (d, &n);
    if (out == NULL)
    {
        return 0;
    }
    if (n > 0 && out[0].width > 0 && out[0].height > 0)
    {
        *x = out[0].x_org;
        *y = out[0].y_org;
        *w = out[0].width;
        *h = out[0].height;
        ok = 1;
    }
    XFree (out);

    return ok;
}

void bench_stage (Display *d, int margin, int *x, int *y, int *w, int *h)
{
    int scr = DefaultScreen (d);
    int screen_x = 0, screen_y = 0;
    int screen_w = DisplayWidth (d, scr);
    int screen_h = DisplayHeight (d, scr);
    int stage_w, stage_h;

    primary_monitor (d, &screen_x, &screen_y, &screen_w, &screen_h);
    stage_w = (screen_w < STAGE_W) ? screen_w : STAGE_W;
    stage_h = (screen_h < STAGE_H) ? screen_h : STAGE_H;

    *x = screen_x + (screen_w - stage_w) / 2 + margin;
    *y = screen_y + (screen_h - stage_h) / 2 + margin;
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
