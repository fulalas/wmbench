#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <X11/Xlib.h>

typedef struct { XID client; unsigned int mask; } IdSpec;
typedef struct { IdSpec spec; long length; void *value; } IdValue;

static int ignore (Display *d, XErrorEvent *e) { (void) d; (void) e; return 0; }

static pid_t client_pid (Display *d, XID client)
{
    Status (*query) (Display *, long, IdSpec *, long *, IdValue **);
    pid_t (*get_pid) (IdValue *);
    void (*destroy) (long, IdValue *);
    IdSpec spec = { client, 1 << 1 };
    IdValue *ids = NULL;
    long n = 0, i;
    pid_t pid = -1;
    int op, ev, err;
    void *lib;

    if (!XQueryExtension (d, "X-Resource", &op, &ev, &err)) return -1;
    if (!(lib = dlopen ("libXRes.so.1", RTLD_NOW))) return -1;
    query = (Status (*) (Display *, long, IdSpec *, long *, IdValue **)) dlsym (lib, "XResQueryClientIds");
    get_pid = (pid_t (*) (IdValue *)) dlsym (lib, "XResGetClientPid");
    destroy = (void (*) (long, IdValue *)) dlsym (lib, "XResClientIdsDestroy");
    if (query && get_pid && destroy)
    {
        XSetErrorHandler (ignore);
        if (query (d, 1, &spec, &n, &ids) == Success)
        {
            for (i = 0; i < n && pid < 0; i++)
                pid = get_pid (&ids[i]);
            destroy (n, ids);
        }
    }
    dlclose (lib);
    return pid;
}

int main (int argc, char **argv)
{
    Display *d = XOpenDisplay (NULL);
    char name[32];
    XID client;
    pid_t pid;

    if (!d) return 2;
    if (argc > 1)
        client = strtoul (argv[1], NULL, 0);
    else
    {
        snprintf (name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen (d));
        client = XGetSelectionOwner (d, XInternAtom (d, name, False));
    }
    if (client == None) return 1;
    pid = client_pid (d, client);
    if (pid >= 0) printf ("%d\n", (int) pid);
    return 0;
}
