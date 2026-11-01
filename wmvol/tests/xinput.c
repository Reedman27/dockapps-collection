/* tiny XTest driver: xinput move X Y | click B | press B | release B | key NAME | sleep MS  (sequence of commands) */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "move")) { XTestFakeMotionEvent(d, -1, atoi(argv[i+1]), atoi(argv[i+2]), 0); i += 2; }
        else if (!strcmp(argv[i], "click")) { int b = atoi(argv[++i]); XTestFakeButtonEvent(d, b, True, 0); XTestFakeButtonEvent(d, b, False, 0); }
        else if (!strcmp(argv[i], "press")) XTestFakeButtonEvent(d, atoi(argv[++i]), True, 0);
        else if (!strcmp(argv[i], "release")) XTestFakeButtonEvent(d, atoi(argv[++i]), False, 0);
        else if (!strcmp(argv[i], "esc")) { KeyCode k = XKeysymToKeycode(d, XK_Escape); XTestFakeKeyEvent(d, k, True, 0); XTestFakeKeyEvent(d, k, False, 0); }
        else if (!strcmp(argv[i], "ctrl")) { KeyCode k = XKeysymToKeycode(d, XK_Control_L); XTestFakeKeyEvent(d, k, !strcmp(argv[i+1], "down"), 0); i++; }
        else if (!strcmp(argv[i], "sleep")) { XFlush(d); usleep(atoi(argv[++i]) * 1000); }
        XFlush(d);
        usleep(30000);
    }
    XSync(d, False);
    return 0;
}
