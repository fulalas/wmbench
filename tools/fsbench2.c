/*
 * A fullscreen or windowed OpenGL benchmark in an ordinary window.
 *
 * It never sets _NET_WM_BYPASS_COMPOSITOR and is not an override redirect
 * window, so it is the kind of application a compositor keeps compositing for.
 * Windowed is the interesting mode for comparing renderers, since then every
 * frame it draws has to be composited.
 *
 *   fsbench <seconds> [windowed] [target fps]
 *
 * BENCH_BYPASS=1 sets _NET_WM_BYPASS_COMPOSITOR on the window, which is how
 * a player or a game asks the compositor to step aside while it is
 * fullscreen. Without it, whether compositing stops is the window manager's
 * own decision.
 *
 * A target frame rate holds the application steady, so two renderers can be
 * given exactly the same amount of work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <epoxy/gl.h>
#include <epoxy/glx.h>
#include "polite.h"
#include "gate.h"
#include "now.h"
#include "stage.h"

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

int main (int argc, char **argv)
{
    double seconds = (argc > 1) ? atof (argv[1]) : 10.0;
    double target = (argc > 3) ? atof (argv[3]) : 0.0;
    double frame_budget = (target > 0.0) ? 1.0 / target : 0.0;
    double next_frame = 0.0;
    Display *d = XOpenDisplay (NULL);
    int scr, attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                           GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    Window win;
    GLXContext ctx;
    Atom wm_state, fullscreen, wtype, wtype_normal;
    XEvent ev;
    GLuint prog;
    GLint u_t;
    double start, last, mstart;
    long frames, window_frames;
    int wx, wy, ww, wh;
    long tasks, warm, done = 0;

    if (d == NULL) { fprintf (stderr, "no display\n"); return 1; }
    scr = DefaultScreen (d);
    vi = glXChooseVisual (d, scr, attribs);
    if (vi == NULL) { fprintf (stderr, "no visual\n"); return 1; }

    memset (&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap (d, RootWindow (d, scr), vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    /*
     * A fullscreen test is created already covering the monitor. It matters:
     * a window manager decides whether to leave a window out of compositing
     * when it maps it, and asks whether the window is fullscreen by looking
     * at its geometry. A window that is mapped at some other size and only
     * then asks for fullscreen is past that decision for good - which is why
     * asking to bypass compositing appeared to change nothing at all.
     */
    if (!(argc > 2 && !strcmp (argv[2], "windowed")))
    {
        wx = 0;
        wy = 0;
        ww = DisplayWidth (d, scr);
        wh = DisplayHeight (d, scr);
    }
    else
    {
        int room_w, room_h;

        /* Inside the area the benchmark may use, wherever that is */
        bench_stage (d, 60, &wx, &wy, &room_w, &room_h);
        ww = (argc > 4) ? atoi (argv[4]) : 2400;
        wh = (argc > 5) ? atoi (argv[5]) : 1400;
        if (ww > room_w) ww = room_w;
        if (wh > room_h) wh = room_h;
    }
    win = XCreateWindow (d, RootWindow (d, scr), wx, wy, (unsigned) ww,
                         (unsigned) wh, 0, vi->depth,
                         InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XStoreName (d, win, "fsbench");

    wtype = XInternAtom (d, "_NET_WM_WINDOW_TYPE", False);
    wtype_normal = XInternAtom (d, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty (d, win, wtype, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *) &wtype_normal, 1);

    /*
     * Both of these have to be set before the window is mapped. A window
     * manager decides whether to leave a window out of compositing when it
     * maps it, and one was found to store a hint that arrives later without
     * ever acting on it, so a test that asked after mapping was never granted anything and
     * looked exactly like a test that never asked.
     */
    if (getenv ("BENCH_BYPASS") != NULL)
    {
        /* What mpv and SDL do: ask for compositing to be left out of the way */
        Atom bypass = XInternAtom (d, "_NET_WM_BYPASS_COMPOSITOR", False);
        long one = 1;

        XChangeProperty (d, win, bypass, XA_CARDINAL, 32, PropModeReplace,
                         (unsigned char *) &one, 1);
    }
    if (!(argc > 2 && !strcmp (argv[2], "windowed")))
    {
        Atom st = XInternAtom (d, "_NET_WM_STATE", False);
        Atom fs = XInternAtom (d, "_NET_WM_STATE_FULLSCREEN", False);

        XChangeProperty (d, win, st, XA_ATOM, 32, PropModeReplace,
                         (unsigned char *) &fs, 1);
    }

    XMapWindow (d, win);
    XFlush (d);

    if (!(argc > 2 && !strcmp (argv[2], "windowed")))
    {
        wm_state = XInternAtom (d, "_NET_WM_STATE", False);
        fullscreen = XInternAtom (d, "_NET_WM_STATE_FULLSCREEN", False);
        memset (&ev, 0, sizeof ev);
        ev.type = ClientMessage;
        ev.xclient.window = win;
        ev.xclient.message_type = wm_state;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 1;
        ev.xclient.data.l[1] = fullscreen;
        ev.xclient.data.l[3] = 1;
        XSendEvent (d, RootWindow (d, scr), False,
                    SubstructureNotifyMask | SubstructureRedirectMask, &ev);
        XFlush (d);
        /*
         * Compositing is only suspended for the window that has the focus,
         * so a fullscreen test has to ask for it - politely, or a window
         * manager calls it focus stealing and does nothing.
         */
        polite_activate (d, RootWindow (d, scr), win);
    }

    ctx = glXCreateContext (d, vi, NULL, True);
    glXMakeCurrent (d, win, ctx);
    /* Not everywhere; without the guard epoxy aborts where it is missing */
    if (epoxy_has_glx_extension (d, scr, "GLX_EXT_swap_control"))
    {
        glXSwapIntervalEXT (d, win, 0);
    }

    prog = glCreateProgram ();
    glAttachShader (prog, compile (GL_VERTEX_SHADER, vs_src));
    glAttachShader (prog, compile (GL_FRAGMENT_SHADER,
                                   (getenv ("BENCH_LIGHT") != NULL)
                                   ? fs_light : fs_src));
    glLinkProgram (prog);
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
        XWindowAttributes wa;
        double t = bench_now ();

        while (XPending (d))
        {
            XNextEvent (d, &ev);
        }
        XGetWindowAttributes (d, win, &wa);
        glViewport (0, 0, wa.width, wa.height);
        glUniform1f (u_t, (GLfloat) (t - start));
        glBegin (GL_QUADS);
        glVertex2f (-1.0f, -1.0f);
        glVertex2f ( 1.0f, -1.0f);
        glVertex2f ( 1.0f,  1.0f);
        glVertex2f (-1.0f,  1.0f);
        glEnd ();
        glXSwapBuffers (d, win);
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
    glXMakeCurrent (d, None, NULL);
    glXDestroyContext (d, ctx);
    XDestroyWindow (d, win);
    XCloseDisplay (d);
    return 0;
}
