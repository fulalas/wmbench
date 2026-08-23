#include <stdio.h>
#include <X11/Xlib.h>
int main (void)
{
    Display *d = XOpenDisplay (NULL);
    char name[32];
    Window o;

    if (!d) return 2;
    snprintf (name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen (d));
    o = XGetSelectionOwner (d, XInternAtom (d, name, False));
    printf ("%s owner: %s\n", name, o == None ? "none" : "present");
    return o == None ? 1 : 0;
}
