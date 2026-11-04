#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "protocols/xdg-shell-client-protocol.h"
#include "protocols/presentation-time-client-protocol.h"

#define WIDTH 400
#define HEIGHT 300
#define STRIDE (WIDTH * 4)
#define BUFFER_BYTES (STRIDE * HEIGHT)
#define NBUFFERS 3
#define WARMUP_SECONDS 1.0
#define MAX_SAMPLES 100000
#define NOT_DONE 3

typedef struct {
    struct wl_buffer *wl;
    uint32_t *pixels;
    int busy;
} buffer;

typedef struct {
    struct wp_presentation_feedback *feedback;
    double committed;
} pending;

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_presentation *presentation;
static clockid_t clock_id = CLOCK_MONOTONIC;
static struct wl_surface *surface;
static buffer buffers[NBUFFERS];
static double samples[MAX_SAMPLES];
static int nsamples, discarded, configured, closed;
static double start, stop;
static uint32_t frame_number;

static double now (void)
{
    struct timespec t;

    clock_gettime (clock_id, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void buffer_release (void *data, struct wl_buffer *b)
{
    (void) b;
    ((buffer *) data)->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void feedback_sync_output (void *data, struct wp_presentation_feedback *f,
                                  struct wl_output *o)
{
    (void) data; (void) f; (void) o;
}

static void feedback_presented (void *data, struct wp_presentation_feedback *f,
                                uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                                uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
                                uint32_t flags)
{
    pending *p = data;
    double shown = ((double) (((uint64_t) sec_hi << 32) | sec_lo)) + nsec / 1e9;

    (void) refresh; (void) seq_hi; (void) seq_lo; (void) flags;
    if (p->committed >= start + WARMUP_SECONDS && nsamples < MAX_SAMPLES)
        samples[nsamples++] = (shown - p->committed) * 1000.0;
    wp_presentation_feedback_destroy (f);
    free (p);
}

static void feedback_discarded (void *data, struct wp_presentation_feedback *f)
{
    pending *p = data;

    if (p->committed >= start + WARMUP_SECONDS)
        discarded++;
    wp_presentation_feedback_destroy (f);
    free (p);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    feedback_sync_output, feedback_presented, feedback_discarded
};

static void draw_frame (void);

static void frame_done (void *data, struct wl_callback *cb, uint32_t time)
{
    (void) data; (void) time;
    wl_callback_destroy (cb);
    if (now () < stop)
        draw_frame ();
}

static const struct wl_callback_listener frame_listener = { frame_done };

static buffer *free_buffer (void)
{
    for (int i = 0; i < NBUFFERS; i++)
        if (!buffers[i].busy)
            return &buffers[i];
    return NULL;
}

static void draw_frame (void)
{
    buffer *b = free_buffer ();
    pending *p = calloc (1, sizeof (*p));

    if (b == NULL || p == NULL)
    {
        fprintf (stderr, "latbench: no free buffer\n");
        exit (EXIT_FAILURE);
    }
    uint32_t color = 0xff000000u | ((frame_number++ * 0x3f1f0fu) & 0xffffffu);
    for (int i = 0; i < WIDTH * HEIGHT; i++)
        b->pixels[i] = color;
    b->busy = 1;
    wl_surface_attach (surface, b->wl, 0, 0);
    wl_surface_damage_buffer (surface, 0, 0, WIDTH, HEIGHT);
    wl_callback_add_listener (wl_surface_frame (surface), &frame_listener, NULL);
    p->feedback = wp_presentation_feedback (presentation, surface);
    wp_presentation_feedback_add_listener (p->feedback, &feedback_listener, p);
    p->committed = now ();
    wl_surface_commit (surface);
}

static void presentation_clock (void *data, struct wp_presentation *wp, uint32_t id)
{
    (void) data; (void) wp;
    clock_id = (clockid_t) id;
}

static const struct wp_presentation_listener presentation_listener = { presentation_clock };

static void wm_ping (void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void) data;
    xdg_wm_base_pong (wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void global (void *data, struct wl_registry *r, uint32_t name,
                    const char *iface, uint32_t version)
{
    (void) data; (void) version;
    if (strcmp (iface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind (r, name, &wl_compositor_interface, 4);
    else if (strcmp (iface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind (r, name, &wl_shm_interface, 1);
    else if (strcmp (iface, xdg_wm_base_interface.name) == 0)
    {
        wm_base = wl_registry_bind (r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener (wm_base, &wm_listener, NULL);
    }
    else if (strcmp (iface, wp_presentation_interface.name) == 0)
    {
        presentation = wl_registry_bind (r, name, &wp_presentation_interface, 1);
        wp_presentation_add_listener (presentation, &presentation_listener, NULL);
    }
}

static void global_remove (void *data, struct wl_registry *r, uint32_t name)
{
    (void) data; (void) r; (void) name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void surface_configure (void *data, struct xdg_surface *xs, uint32_t serial)
{
    (void) data;
    xdg_surface_ack_configure (xs, serial);
    configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = { surface_configure };

static void toplevel_configure (void *data, struct xdg_toplevel *t, int32_t w,
                                int32_t h, struct wl_array *states)
{
    (void) data; (void) t; (void) w; (void) h; (void) states;
}

static void toplevel_close (void *data, struct xdg_toplevel *t)
{
    (void) data; (void) t;
    closed = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure, toplevel_close, NULL, NULL
};

static int make_buffers (void)
{
    int fd = memfd_create ("latbench", MFD_CLOEXEC);

    if (fd < 0 || ftruncate (fd, (off_t) BUFFER_BYTES * NBUFFERS) < 0)
        return 0;
    uint8_t *data = mmap (NULL, (size_t) BUFFER_BYTES * NBUFFERS,
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
        return 0;
    struct wl_shm_pool *pool = wl_shm_create_pool (shm, fd, BUFFER_BYTES * NBUFFERS);
    for (int i = 0; i < NBUFFERS; i++)
    {
        buffers[i].wl = wl_shm_pool_create_buffer (pool, i * BUFFER_BYTES, WIDTH,
                                                   HEIGHT, STRIDE,
                                                   WL_SHM_FORMAT_XRGB8888);
        buffers[i].pixels = (uint32_t *) (data + (size_t) i * BUFFER_BYTES);
        wl_buffer_add_listener (buffers[i].wl, &buffer_listener, &buffers[i]);
    }
    wl_shm_pool_destroy (pool);
    close (fd);
    return 1;
}

static int compare (const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;

    return (x > y) - (x < y);
}

static void report (void)
{
    if (nsamples == 0)
    {
        fprintf (stderr, "latbench: no frame was presented\n");
        exit (EXIT_FAILURE);
    }
    qsort (samples, (size_t) nsamples, sizeof (samples[0]), compare);
    printf ("FRAMES %d\n", nsamples);
    printf ("DISCARDED %d\n", discarded);
    printf ("MEDIAN %.2f\n", samples[nsamples / 2]);
    printf ("P90 %.2f\n", samples[(nsamples * 9) / 10]);
}

static void dispatch_until (struct wl_display *display, double deadline)
{
    struct pollfd fd = { wl_display_get_fd (display), POLLIN, 0 };

    while (!closed && now () < deadline)
    {
        while (wl_display_prepare_read (display) != 0)
            wl_display_dispatch_pending (display);
        wl_display_flush (display);
        int ms = (int) ((deadline - now ()) * 1000.0) + 1;
        if (ms < 1)
            ms = 1;
        if (poll (&fd, 1, ms) > 0)
        {
            if (wl_display_read_events (display) < 0)
                return;
        }
        else
            wl_display_cancel_read (display);
        if (wl_display_dispatch_pending (display) < 0)
            return;
    }
}

int main (int argc, char **argv)
{
    double seconds = argc > 1 ? atof (argv[1]) : 10.0;
    const char *backend = getenv ("BENCH_BACKEND");
    struct wl_display *display;

    if (getenv ("WAYLAND_DISPLAY") == NULL || (backend && strcmp (backend, "x11") == 0))
    {
        printf ("NOT DONE: latency is measured on Wayland only\n");
        return NOT_DONE;
    }
    display = wl_display_connect (NULL);
    if (display == NULL)
    {
        fprintf (stderr, "latbench: cannot connect: %s\n", strerror (errno));
        return EXIT_FAILURE;
    }
    wl_registry_add_listener (wl_display_get_registry (display), &registry_listener, NULL);
    wl_display_roundtrip (display);
    wl_display_roundtrip (display);
    if (presentation == NULL)
    {
        printf ("NOT DONE: the compositor has no wp_presentation\n");
        return NOT_DONE;
    }
    if (compositor == NULL || shm == NULL || wm_base == NULL || !make_buffers ())
    {
        fprintf (stderr, "latbench: missing globals or buffers\n");
        return EXIT_FAILURE;
    }
    surface = wl_compositor_create_surface (compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface (wm_base, surface);
    xdg_surface_add_listener (xs, &xdg_surface_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel (xs);
    xdg_toplevel_add_listener (top, &toplevel_listener, NULL);
    xdg_toplevel_set_title (top, "latbench");
    wl_surface_commit (surface);
    while (!configured && wl_display_dispatch (display) != -1)
        ;
    start = now ();
    stop = start + WARMUP_SECONDS + seconds;
    draw_frame ();
    dispatch_until (display, stop + 0.5);
    report ();
    return EXIT_SUCCESS;
}
