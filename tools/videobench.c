/*
 * A video player, which nothing here has covered and which is one of the
 * commonest things a compositor has to composite.
 *
 * The difference from every other benchmark in this directory is where the
 * window's pixels come from. Everything else either renders with OpenGL or
 * draws with X primitives; a player hands over a buffer of pixels it made
 * itself, with XShmPutImage. The pixmap the compositor then samples as a
 * texture has been filled by the CPU, and may not be laid out the way a
 * GPU-rendered one is. If sampling it is slow, this is where the OpenGL
 * renderer would lose.
 *
 *   videobench <seconds> [frames per second] [width] [height]
 *
 * BENCH_TASKS=N hands over N frames instead of running for the seconds given.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XShm.h>

#include "gate.h"
#include "now.h"
#include "place.h"

/*
 * Fixed work: BENCH_TASKS=N does exactly N tasks, however long that takes, so
 * every session performs the same amount of work and the numbers compare. The
 * measured part is bracketed by the two marks, after an unmeasured warm-up.
 */
static long bench_tasks (void)
{
    const char *e = getenv ("BENCH_TASKS");

    return (e != NULL && *e != '\0') ? atol (e) : 0;
}

static void mark (const char *s)
{
    if (strcmp (s, "MEASURE-START") == 0)
    {
        /* In a mix of programs, wait until the whole load is up. See gate.c */
        bench_wait_go ();
    }
    printf ("%s\n", s);
    fflush (stdout);
}

int main (int argc, char **argv)
{
    Display *d;
    Window win, root;
    GC gc;
    XShmSegmentInfo shm;
    XImage *img;
    XSizeHints hints;
    Atom wtype, normal;
    double seconds = (argc > 1) ? atof (argv[1]) : 12.0;
    double rate = (argc > 2) ? atof (argv[2]) : 60.0;

    if (rate <= 0.0)
    {
        rate = 60.0;
    }
    int w = (argc > 3) ? atoi (argv[3]) : 1920;
    int h = (argc > 4) ? atoi (argv[4]) : 1080;

    /* The frame loop writes four pixels at a time */
    w &= ~3;
    int scr, i, x, y, frames = 0, major, minor, warm;
    long tasks, done = 0;
    Bool pixmaps;
    double start, mstart;

    d = XOpenDisplay (NULL);
    if (d == NULL)
    {
        fprintf (stderr, "no display\n");

        return 2;
    }
    if (!XShmQueryVersion (d, &major, &minor, &pixmaps))
    {
        fprintf (stderr, "no XShm\n");

        return 2;
    }
    scr = DefaultScreen (d);
    root = RootWindow (d, scr);

    win = XCreateSimpleWindow (d, root, 100, 100, w, h, 0,
                               BlackPixel (d, scr), BlackPixel (d, scr));
    hints.flags = USPosition | USSize | PPosition | PSize;
    hints.x = 100; hints.y = 100; hints.width = w; hints.height = h;
    XSetWMNormalHints (d, win, &hints);
    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &normal, 1);
    XStoreName (d, win, "videobench");
    XMapWindow (d, win);
    gc = XCreateGC (d, win, 0, NULL);
    XSync (d, False);
    sleep (2);
    bench_placed (d, win, 100, 100, "videobench");

    memset (&shm, 0, sizeof shm);
    img = XShmCreateImage (d, DefaultVisual (d, scr), DefaultDepth (d, scr),
                           ZPixmap, NULL, &shm, w, h);
    if (img == NULL)
    {
        fprintf (stderr, "XShmCreateImage failed\n");

        return 2;
    }
    /* The frame loop writes 32-bit pixels; anything else would overrun */
    if (img->bits_per_pixel != 32)
    {
        fprintf (stderr, "needs a 32-bit visual, this one is %d\n",
                 img->bits_per_pixel);

        return 2;
    }
    shm.shmid = shmget (IPC_PRIVATE, img->bytes_per_line * img->height,
                        IPC_CREAT | 0600);
    if (shm.shmid < 0)
    {
        fprintf (stderr, "shmget failed\n");

        return 2;
    }
    shm.shmaddr = shmat (shm.shmid, NULL, 0);
    if (shm.shmaddr == (void *) -1)
    {
        fprintf (stderr, "shmat failed\n");

        return 2;
    }
    img->data = shm.shmaddr;
    shm.readOnly = False;
    if (!XShmAttach (d, &shm))
    {
        fprintf (stderr, "XShmAttach failed\n");

        return 2;
    }
    XSync (d, False);

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 10 : 0;
    start = bench_now ();
    mstart = start;
    for (i = 0; ; i++)
    {
        double due = start + i / rate;

        /* A cheap moving pattern, the way a decoder hands over a new frame */
        for (y = 0; y < h; y++)
        {
            unsigned int *row = (unsigned int *)
                (img->data + (size_t) y * img->bytes_per_line);
            unsigned int v = (unsigned int) (((y + i * 3) & 0xff) << 8);

            for (x = 0; x < w; x += 4)
            {
                row[x] = v | (unsigned) ((x + i) & 0xff);
                row[x + 1] = v;
                row[x + 2] = v | 0x400000;
                row[x + 3] = v;
            }
        }
        XShmPutImage (d, win, gc, img, 0, 0, 0, 0, w, h, False);
        XSync (d, False);
        frames++;

        if (tasks > 0)
        {
            if (i + 1 == warm)
            {
                mark ("MEASURE-START");
                mstart = bench_now ();
                /*
                 * The gate in mark() can have held this a long time, and the
                 * schedule counts from start: left alone it would run flat out
                 * to catch up, which is not the fixed rate this load is for.
                 */
                start = mstart - (double) (i + 1) / rate;
                frames = 0;
            }
            else if (i + 1 > warm)
            {
                done++;
            }
            if (done >= tasks)
            {
                break;
            }
        }
        else if (bench_now () - start >= seconds)
        {
            break;
        }

        while (bench_now () < due)
        {
            usleep (200);
        }
    }

    if (tasks > 0)
    {
        mark ("MEASURE-END");
        printf ("AVERAGE %.1f frames/s over %.1f s, %ld frames\n",
                frames / (bench_now () - mstart), bench_now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.1f frames/s over %.0f s\n",
                frames / (bench_now () - start), seconds);
    }

    XShmDetach (d, &shm);
    XDestroyImage (img);
    shmdt (shm.shmaddr);
    shmctl (shm.shmid, IPC_RMID, NULL);
    XDestroyWindow (d, win);
    XCloseDisplay (d);

    return 0;
}
