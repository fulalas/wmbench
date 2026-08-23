/*
 * One process owns one ordinary managed GL window.  multi_compare.sh starts
 * eight copies at overlapping positions, which makes a multi-window workload
 * without relying on GLX context switching between windows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <epoxy/gl.h>
#include <epoxy/glx.h>

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 520

static const char *vertex_source =
    "#version 120\n"
    "void main (void) { gl_Position = gl_Vertex; }\n";

static const char *fragment_source =
    "#version 120\n"
    "uniform float t;\n"
    "uniform float seed;\n"
    "void main (void)\n"
    "{\n"
    "    vec2 p = gl_FragCoord.xy * vec2 (0.013, 0.017);\n"
    "    float v = seed + t;\n"
    "    for (int i = 0; i < 90; i++)\n"
    "    {\n"
    "        v = fract (sin (v + p.x * 1.71 + p.y * 2.13) * 43758.5453);\n"
    "        p = p.yx * 1.013 + vec2 (v, v * 0.73);\n"
    "    }\n"
    "    gl_FragColor = vec4 (v, fract (v + seed), fract (v + t), 1.0);\n"
    "}\n";

static double
now (void)
{
    struct timespec ts;

    clock_gettime (CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint
compile_shader (GLenum type, const char *source)
{
    GLuint shader = glCreateShader (type);
    GLint ok = GL_FALSE;

    glShaderSource (shader, 1, &source, NULL);
    glCompileShader (shader);
    glGetShaderiv (shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];

        glGetShaderInfoLog (shader, sizeof log, NULL, log);
        fprintf (stderr, "shader compile failed: %s\n", log);
        exit (1);
    }

    return shader;
}

static GLuint
make_program (void)
{
    GLuint program = glCreateProgram ();
    GLuint vertex = compile_shader (GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader (GL_FRAGMENT_SHADER, fragment_source);
    GLint ok = GL_FALSE;

    glAttachShader (program, vertex);
    glAttachShader (program, fragment);
    glLinkProgram (program);
    glGetProgramiv (program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];

        glGetProgramInfoLog (program, sizeof log, NULL, log);
        fprintf (stderr, "program link failed: %s\n", log);
        exit (1);
    }
    glDeleteShader (vertex);
    glDeleteShader (fragment);

    return program;
}

int
main (int argc, char **argv)
{
    const int index = (argc > 1) ? atoi (argv[1]) : 0;
    const double seconds = (argc > 2) ? atof (argv[2]) : 20.0;
    Display *dpy;
    int screen;
    int attributes[] = { GLX_RGBA, GLX_DOUBLEBUFFER,
                         GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                         GLX_BLUE_SIZE, 8, None };
    XVisualInfo *visual;
    XSetWindowAttributes swa;
    Window window;
    GLXContext context;
    Atom window_type, normal_type;
    GLuint program;
    GLint time_uniform, seed_uniform;
    double start, last;
    long frames = 0;

    dpy = XOpenDisplay (NULL);
    if (dpy == NULL)
    {
        fprintf (stderr, "cannot open display\n");
        return 1;
    }
    screen = DefaultScreen (dpy);
    visual = glXChooseVisual (dpy, screen, attributes);
    if (visual == NULL)
    {
        fprintf (stderr, "cannot choose an OpenGL visual\n");
        return 1;
    }

    memset (&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap (dpy, RootWindow (dpy, screen),
                                    visual->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    window_type = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE", False);
    normal_type = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    window = XCreateWindow (dpy, RootWindow (dpy, screen),
                            160 + (index % 4) * 560,
                            160 + (index / 4) * 430,
                            WINDOW_WIDTH, WINDOW_HEIGHT, 0,
                            visual->depth, InputOutput, visual->visual,
                            CWColormap | CWEventMask, &swa);
    XStoreName (dpy, window, "xfwm4 multi-window GPU workload");
    XChangeProperty (dpy, window, window_type, XA_ATOM, 32,
                     PropModeReplace, (unsigned char *) &normal_type, 1);
    XMapWindow (dpy, window);
    XFlush (dpy);

    context = glXCreateContext (dpy, visual, NULL, True);
    if (context == NULL || !glXMakeCurrent (dpy, window, context))
    {
        fprintf (stderr, "cannot create an OpenGL context\n");
        return 1;
    }
    /* Not everywhere; without the guard epoxy aborts where it is missing */
    if (epoxy_has_glx_extension (dpy, screen, "GLX_EXT_swap_control"))
    {
        glXSwapIntervalEXT (dpy, window, 0);
    }
    program = make_program ();
    time_uniform = glGetUniformLocation (program, "t");
    seed_uniform = glGetUniformLocation (program, "seed");
    glUseProgram (program);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_BLEND);

    {
        struct timespec settle = { 1, 0 };

        nanosleep (&settle, NULL);
    }

    start = now ();
    last = 0.0;
    while (now () - start < seconds)
    {
        double t = now () - start;

        while (XPending (dpy))
        {
            XEvent event;

            XNextEvent (dpy, &event);
        }
        glViewport (0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
        glUniform1f (time_uniform, (GLfloat) t);
        glUniform1f (seed_uniform, (GLfloat) (index * 0.37));
        glBegin (GL_QUADS);
        glVertex2f (-1.0f, -1.0f);
        glVertex2f (1.0f, -1.0f);
        glVertex2f (1.0f, 1.0f);
        glVertex2f (-1.0f, 1.0f);
        glEnd ();
        glXSwapBuffers (dpy, window);
        glFinish ();
        frames++;

        /* t and last both count seconds since start */
        if (t - last >= 2.0)
        {
            printf ("  window %d %.2f fps\n", index, frames / t);
            fflush (stdout);
            last = t;
        }
    }

    printf ("AVERAGE %ld frames over %.0f s\n", frames, seconds);
    glDeleteProgram (program);
    glXMakeCurrent (dpy, None, NULL);
    glXDestroyContext (dpy, context);
    XDestroyWindow (dpy, window);
    XFree (visual);
    XCloseDisplay (dpy);

    return 0;
}
