/*
 *   fsbench2 <seconds> [windowed] [target fps] [width] [height]
 *
 * An ordinary window, so it is the kind of application a compositor keeps
 * compositing for. A target frame rate holds it steady, so two sessions are
 * given exactly the same amount of work.
 *
 * BENCH_BYPASS=1 sets _NET_WM_BYPASS_COMPOSITOR, which is how a player asks
 * compositing to step aside while it is fullscreen. Wayland has no such
 * property - there the compositor decides by itself, and will not while it has
 * to be told what is behind the surface - so BENCH_BYPASS declares the whole
 * surface opaque instead, which is what makes it eligible.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <epoxy/gl.h>
#include <epoxy/glx.h>
#include <epoxy/egl.h>
#include <wayland-egl.h>
#include "gate.h"
#include "now.h"
#include "win.h"
#include "place.h"

static const char *vs_src =
    "void main (void) { gl_Position = gl_Vertex; }\n";

/* Heavy on purpose: the card must be the bottleneck, not the protocol. */
/*
 * BENCH_LIGHT=1: a fragment shader that costs almost nothing, so the
 * application is not its own bottleneck. The heavy one below hides what
 * compositing costs: at 4K it takes some 14 ms a frame, while compositing a
 * fullscreen window is well under one, so leaving compositing out of the way
 * changes the frame rate by a percent or two and looks like it did nothing.
 */
static const char *fs_light =
    "uniform float t;\n"
    "void main (void)\n"
    "{\n"
    "    gl_FragColor = vec4 (fract (gl_FragCoord.x * 0.001 + t),\n"
    "                         fract (gl_FragCoord.y * 0.001), 0.5, 1.0);\n"
    "}\n";

static const char *fs_src =
    "uniform float t;\n"
    "void main (void)\n"
    "{\n"
    "    vec2 p = gl_FragCoord.xy * 0.01;\n"
    "    float a = 0.0;\n"
    "    for (int i = 0; i < 340; i++)\n"
    "    {\n"
    "        a += sin (p.x + a + t) * cos (p.y - a);\n"
    "        a = fract (a * 1.37);\n"
    "    }\n"
    "    gl_FragColor = vec4 (a, a * 0.5, 1.0 - a, 1.0);\n"
    "}\n";

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

static GLuint compile (GLenum type, const char *src)
{
    GLuint s = glCreateShader (type);
    GLint ok = 0;
    glShaderSource (s, 1, &src, NULL);
    glCompileShader (s);
    glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog (s, sizeof log, NULL, log);
        fprintf (stderr, "shader: %s\n", log);
        exit (1);
    }
    return s;
}

static Display *xd;
static Window xwin;
static GLXContext glx_ctx;
static bw_win *wlwin;
static struct wl_egl_window *egl_win;
static EGLDisplay egl_dpy;
static EGLSurface egl_surf;
static EGLContext egl_ctx;

int main (int argc, char **argv)
{
    double seconds = (argc > 1) ? atof (argv[1]) : 10.0;
    double target = (argc > 3) ? atof (argv[3]) : 0.0;
    double frame_budget = (target > 0.0) ? 1.0 / target : 0.0;
    double next_frame = 0.0;
    int windowed = (argc > 2 && !strcmp (argv[2], "windowed"));
    int on_wayland;
    XEvent ev;
    GLuint prog;
    GLint u_t;
    double start, last, mstart;
    long frames, window_frames;
    /* ww and wh are the window's logical size, fw and fh the pixels a frame
       really holds - the same on X11, the logical size times the output scale
       on Wayland. The buffer and the viewport are sized in the second. */
    int wx, wy, ww, wh, fw, fh;
    long tasks, warm, done = 0;

    if (!bw_open ())
    {
        fprintf (stderr, "no display\n");

        return 1;
    }
    on_wayland = bw_is_wayland ();

    /*
     * A fullscreen test is created already covering the monitor. It matters:
     * a window manager decides whether to leave a window out of compositing
     * when it maps it, and asks whether the window is fullscreen by looking
     * at its geometry. A window that is mapped at some other size and only
     * then asks for fullscreen is past that decision for good - which is why
     * asking to bypass compositing appeared to change nothing at all.
     */
    if (!windowed)
    {
        wx = 0;
        wy = 0;
        bw_screen_size (&ww, &wh);
    }
    else
    {
        int room_w, room_h;

        /* Inside the area the benchmark may use, wherever that is */
        bw_stage (STAGE_MARGIN, &wx, &wy, &room_w, &room_h);
        ww = (argc > 4) ? atoi (argv[4]) : 2400;
        wh = (argc > 5) ? atoi (argv[5]) : 1400;
        if (ww > room_w) ww = room_w;
        if (wh > room_h) wh = room_h;
    }

    if (on_wayland)
    {
        EGLint cfg_attribs[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                 EGL_BLUE_SIZE, 8,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                 EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
        EGLConfig cfg;
        EGLint ncfg = 0;

        wlwin = bw_create (NULL, wx, wy, ww, wh, "fsbench", BW_GL);
        if (wlwin == NULL)
        {
            fprintf (stderr, "no window\n");

            return 1;
        }
        if (!windowed)
        {
            bw_fullscreen (wlwin, 1);
        }
        bw_map (wlwin);
        /* The compositor may have answered fullscreen with its own size */
        bw_where (wlwin, NULL, NULL, &ww, &wh);
        bw_frame_size (wlwin, &fw, &fh);
        /* Declared after the configure, or the region would be the size that
           was asked for rather than the one the compositor gave */
        if (getenv ("BENCH_BYPASS") != NULL)
        {
            bw_opaque_region (wlwin, 0, 0, ww, wh);
        }

        egl_dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_WAYLAND_EXT,
                                            bw_native_display (), NULL);
        if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize (egl_dpy, NULL, NULL))
        {
            fprintf (stderr, "no EGL display\n");

            return 1;
        }
        eglBindAPI (EGL_OPENGL_API);
        if (!eglChooseConfig (egl_dpy, cfg_attribs, &cfg, 1, &ncfg) || ncfg == 0)
        {
            fprintf (stderr, "no EGL config\n");

            return 1;
        }
        /* Device pixels: a logical-sized buffer under a scale of 2 would put
           the window on screen at half the size the X11 run gives it, and the
           frame rate would be for a quarter of the pixels */
        egl_win = wl_egl_window_create (bw_native_surface (wlwin), fw, fh);
        egl_surf = eglCreateWindowSurface (egl_dpy, cfg,
                                           (EGLNativeWindowType) egl_win, NULL);
        egl_ctx = eglCreateContext (egl_dpy, cfg, EGL_NO_CONTEXT, NULL);
        if (egl_surf == EGL_NO_SURFACE || egl_ctx == EGL_NO_CONTEXT ||
            !eglMakeCurrent (egl_dpy, egl_surf, egl_surf, egl_ctx))
        {
            fprintf (stderr, "no EGL context\n");

            return 1;
        }
        eglSwapInterval (egl_dpy, 0);
    }
    else
    {
        int scr, attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                               GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
        XVisualInfo *vi;
        XSetWindowAttributes swa;
        Atom wtype, wtype_normal;

        xd = XOpenDisplay (NULL);
        if (xd == NULL)
        {
            fprintf (stderr, "no display\n");

            return 1;
        }
        scr = DefaultScreen (xd);
        vi = glXChooseVisual (xd, scr, attribs);
        if (vi == NULL)
        {
            fprintf (stderr, "no visual\n");

            return 1;
        }

        memset (&swa, 0, sizeof swa);
        swa.colormap = XCreateColormap (xd, RootWindow (xd, scr), vi->visual,
                                        AllocNone);
        swa.event_mask = StructureNotifyMask;
        xwin = XCreateWindow (xd, RootWindow (xd, scr), wx, wy, (unsigned) ww,
                              (unsigned) wh, 0, vi->depth, InputOutput,
                              vi->visual, CWColormap | CWEventMask, &swa);
        /* X11 has no scale of its own: a window's pixels are its size */
        fw = ww;
        fh = wh;
        XStoreName (xd, xwin, "fsbench");

        wtype = XInternAtom (xd, "_NET_WM_WINDOW_TYPE", False);
        wtype_normal = XInternAtom (xd, "_NET_WM_WINDOW_TYPE_NORMAL", False);
        XChangeProperty (xd, xwin, wtype, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *) &wtype_normal, 1);

        /*
         * Both of these have to be set before the window is mapped. A window
         * manager decides whether to leave a window out of compositing when it
         * maps it, and one was found to store a hint that arrives later without
         * ever acting on it, so a test that asked after mapping was never
         * granted anything and looked exactly like a test that never asked.
         */
        if (getenv ("BENCH_BYPASS") != NULL)
        {
            /* What mpv and SDL do: ask for compositing to step aside */
            Atom bypass = XInternAtom (xd, "_NET_WM_BYPASS_COMPOSITOR", False);
            long one = 1;

            XChangeProperty (xd, xwin, bypass, XA_CARDINAL, 32, PropModeReplace,
                             (unsigned char *) &one, 1);
        }
        if (!windowed)
        {
            Atom st = XInternAtom (xd, "_NET_WM_STATE", False);
            Atom fs = XInternAtom (xd, "_NET_WM_STATE_FULLSCREEN", False);

            XChangeProperty (xd, xwin, st, XA_ATOM, 32, PropModeReplace,
                             (unsigned char *) &fs, 1);
        }

        XMapWindow (xd, xwin);
        XFlush (xd);

        if (!windowed)
        {
            Atom wm_state = XInternAtom (xd, "_NET_WM_STATE", False);
            Atom fullscreen = XInternAtom (xd, "_NET_WM_STATE_FULLSCREEN",
                                           False);

            memset (&ev, 0, sizeof ev);
            ev.type = ClientMessage;
            ev.xclient.window = xwin;
            ev.xclient.message_type = wm_state;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = 1;
            ev.xclient.data.l[1] = fullscreen;
            ev.xclient.data.l[3] = 1;
            XSendEvent (xd, RootWindow (xd, scr), False,
                        SubstructureNotifyMask | SubstructureRedirectMask, &ev);
            XFlush (xd);
            /*
             * Compositing is only suspended for the window that has the focus,
             * so a fullscreen test has to ask for it - politely, with the
             * server's own time fished out of a PropertyNotify, or a window
             * manager calls it focus stealing and does nothing.
             */
            {
                XClientMessageEvent aev;
                XEvent tev;
                Atom stamp = XInternAtom (xd, "_BENCH_TIME", False);
                Time now = CurrentTime;

                XSelectInput (xd, xwin,
                              StructureNotifyMask | PropertyChangeMask);
                XChangeProperty (xd, xwin, stamp, XA_STRING, 8, PropModeAppend,
                                 (unsigned char *) "", 0);
                XSync (xd, False);
                while (XCheckTypedWindowEvent (xd, xwin, PropertyNotify, &tev))
                {
                    if (tev.xproperty.atom == stamp)
                    {
                        now = tev.xproperty.time;
                    }
                }
                XSelectInput (xd, xwin, StructureNotifyMask);

                memset (&aev, 0, sizeof aev);
                aev.type = ClientMessage;
                aev.window = xwin;
                aev.message_type = XInternAtom (xd, "_NET_ACTIVE_WINDOW", False);
                aev.format = 32;
                aev.data.l[0] = 2;      /* a pager asks */
                aev.data.l[1] = (long) now;
                XSendEvent (xd, RootWindow (xd, scr), False,
                            SubstructureNotifyMask | SubstructureRedirectMask,
                            (XEvent *) &aev);
                XSync (xd, False);
            }
        }

        glx_ctx = glXCreateContext (xd, vi, NULL, True);
        if (glx_ctx == NULL || !glXMakeCurrent (xd, xwin, glx_ctx))
        {
            fprintf (stderr, "no GL context\n");

            return 1;
        }
        /* Not everywhere; without the guard epoxy aborts where it is missing */
        if (epoxy_has_glx_extension (xd, scr, "GLX_EXT_swap_control"))
        {
            glXSwapIntervalEXT (xd, xwin, 0);
        }
        XFree (vi);
    }

    prog = glCreateProgram ();
    glAttachShader (prog, compile (GL_VERTEX_SHADER, vs_src));
    glAttachShader (prog, compile (GL_FRAGMENT_SHADER,
                                   (getenv ("BENCH_LIGHT") != NULL)
                                   ? fs_light : fs_src));
    glLinkProgram (prog);
    /*
     * A failed link leaves the fixed-function pipeline drawing the quad for
     * nothing, and the run still prints an AVERAGE - a fast, meaningless one.
     */
    {
        GLint ok = 0;

        glGetProgramiv (prog, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[2048];

            glGetProgramInfoLog (prog, sizeof log, NULL, log);
            fprintf (stderr, "link: %s\n", log);

            return 1;
        }
    }
    glUseProgram (prog);
    u_t = glGetUniformLocation (prog, "t");

    printf ("renderer: %s\n", (const char *) glGetString (GL_RENDERER));
    fflush (stdout);

    tasks = bench_tasks ();
    warm = (tasks > 0) ? 120 : 0;       /* shaders and the first frames */
    start = last = mstart = bench_now ();
    frames = window_frames = 0;
    for (;;)
    {
        double t = bench_now ();

        if (on_wayland)
        {
            int nw, nh;

            /*
             * The compositor's configures arrive through the window layer;
             * asked of it here rather than of the server, so no round trip
             * lands in the frame rate being compared.
             */
            bw_pump ();
            bw_where (wlwin, NULL, NULL, &nw, &nh);
            if (nw != ww || nh != wh)
            {
                ww = nw;
                wh = nh;
                bw_frame_size (wlwin, &fw, &fh);
                wl_egl_window_resize (egl_win, fw, fh, 0, 0);
                /* A region left at the old size is a claim about pixels the
                   surface no longer has, and the compositor acts on it */
                if (getenv ("BENCH_BYPASS") != NULL)
                {
                    bw_opaque_region (wlwin, 0, 0, ww, wh);
                }
            }
        }
        else
        {
            while (XPending (xd))
            {
                XNextEvent (xd, &ev);
                /*
                 * Asking the server for the size every frame is a round trip
                 * to the very desktop under test, so its cost lands in the
                 * frame rate being compared. StructureNotifyMask is selected:
                 * the size arrives here instead, for nothing.
                 */
                if (ev.type == ConfigureNotify)
                {
                    ww = ev.xconfigure.width;
                    wh = ev.xconfigure.height;
                    fw = ww;
                    fh = wh;
                }
            }
        }
        glViewport (0, 0, fw, fh);
        glUniform1f (u_t, (GLfloat) (t - start));
        glBegin (GL_QUADS);
        glVertex2f (-1.0f, -1.0f);
        glVertex2f ( 1.0f, -1.0f);
        glVertex2f ( 1.0f,  1.0f);
        glVertex2f (-1.0f,  1.0f);
        glEnd ();
        if (on_wayland)
        {
            eglSwapBuffers (egl_dpy, egl_surf);
        }
        else
        {
            glXSwapBuffers (xd, xwin);
        }
        frames++;
        window_frames++;

        if (tasks > 0)
        {
            if (frames == warm)
            {
                mark ("MEASURE-START");
                mstart = bench_now ();
                /* The gate in mark() may have held us; pace from here on */
                next_frame = 0.0;
            }
            if (frames >= warm + tasks)
            {
                done = frames - warm;
                break;
            }
        }
        else if (bench_now () - start >= seconds)
        {
            break;
        }

        if (frame_budget > 0.0)
        {
            double due;

            if (next_frame == 0.0)
            {
                next_frame = t;
            }
            next_frame += frame_budget;
            due = next_frame - bench_now ();
            if (due > 0.0)
            {
                struct timespec ts;

                ts.tv_sec = (time_t) due;
                ts.tv_nsec = (long) ((due - ts.tv_sec) * 1e9);
                nanosleep (&ts, NULL);
            }
            else
            {
                next_frame = bench_now ();
            }
        }

        if (t - last >= 2.0)
        {
            printf ("  %.1f fps\n", window_frames / (t - last));
            fflush (stdout);
            last = t;
            window_frames = 0;
        }
    }

    if (tasks > 0)
    {
        mark ("MEASURE-END");
        printf ("AVERAGE %.2f fps over %.1f s, %ld frames\n",
                done / (bench_now () - mstart), bench_now () - mstart, done);
    }
    else
    {
        printf ("AVERAGE %.2f fps over %.0f s\n", frames / (bench_now () - start),
                seconds);
    }
    if (on_wayland)
    {
        eglMakeCurrent (egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext (egl_dpy, egl_ctx);
        eglDestroySurface (egl_dpy, egl_surf);
        wl_egl_window_destroy (egl_win);
        bw_destroy (wlwin);
    }
    else
    {
        glXMakeCurrent (xd, None, NULL);
        glXDestroyContext (xd, glx_ctx);
        XDestroyWindow (xd, xwin);
        XCloseDisplay (xd);
    }
    bw_close ();
    return 0;
}
