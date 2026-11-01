/* SPDX-License-Identifier: MIT */
/*
 * wmvol - Window Maker dockapp: PipeWire volume + device switcher
 *
 * A 64x64 dock tile showing the default output volume, and a native X11
 * popup (click the tile) with output/input volume sliders, mute buttons and
 * default device pickers - the same job as KDE's audio applet.
 *
 * Backend: wpctl (WirePlumber).  Refresh: event driven via `pactl subscribe`
 * (pipewire-pulse) with a slow polling fallback.  All subprocesses are
 * non-blocking, the UI never waits on audio.
 *
 * Deps: libX11, libXext, libXft.   License: MIT
 *
 * NOTE: this program was written by an AI (Claude, by Anthropic) at the request
 * of a human maintainer. See the "Made with AI" section of README.md.
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/shape.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.0.1"
#define TILE 64
#define MAXDEV 16

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */
static struct {
    const char *display;
    int windowed;
    int step;           /* wheel step, percent */
    int maxpct;         /* volume limit, percent */
    const char *mixer;  /* command for "open mixer" */
    const char *font;   /* Xft font pattern */
    const char *dump;   /* debug: write PPM screenshots and exit */
    unsigned fg;        /* LCD colour, 0xRRGGBB */
} cfg = { NULL, 0, 5, 100, "pavucontrol", "sans-10", NULL, 0x20b2aa };

static volatile sig_atomic_t quit_flag;
static void on_sig(int s) { (void)s; quit_flag = 1; }

/* Called in a freshly forked child, before exec.
 * - Signal dispositions: SIG_IGN survives exec, and an ignored SIGPIPE keeps a
 *   child such as `pactl subscribe` alive forever once we are gone.
 * - die_with_parent: if we crash or X goes away, take the helper down too. */
static void child_setup(pid_t parent, int die_with_parent)
{
    signal(SIGPIPE, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    if (die_with_parent) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(0);      /* parent died before prctl */
    }
}

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* audio model                                                         */
/* ------------------------------------------------------------------ */
typedef struct { int id; char name[128]; double vol; int muted; int def; } Dev;
typedef struct { Dev d[MAXDEV]; int n; } DevList;

static DevList lists[2];         /* 0 = sinks (output), 1 = sources (input) */
static int pw_ok;                /* last status call succeeded */
static unsigned gen;             /* bumped on every user change */

static Dev *defdev(int sec)
{
    DevList *l = &lists[sec];
    for (int i = 0; i < l->n; i++)
        if (l->d[i].def) return &l->d[i];
    return NULL;
}

static void parse_dev_line(DevList *l, const char *line)
{
    const char *q = line;
    int def = 0;
    while (*q && !isdigit((unsigned char)*q)) { if (*q == '*') def = 1; q++; }
    if (!*q) return;
    char *end;
    long id = strtol(q, &end, 10);
    if (*end != '.' || l->n >= MAXDEV) return;
    q = end + 1;
    while (*q == ' ') q++;
    const char *v = strstr(q, "[vol:");
    size_t nl = v ? (size_t)(v - q) : strlen(q);
    while (nl > 0 && q[nl - 1] == ' ') nl--;
    Dev *d = &l->d[l->n++];
    memset(d, 0, sizeof *d);
    d->id = (int)id;
    d->def = def;
    if (nl >= sizeof d->name) nl = sizeof d->name - 1;
    memcpy(d->name, q, nl);
    d->name[nl] = 0;
    if (v) {
        char *r;
        d->vol = strtod(v + 5, &r);
        d->muted = strstr(r, "MUTED") != NULL;
    }
}

/* Parses `wpctl status`. Returns 1 if an Audio section was found. */
static int parse_status(const char *txt)
{
    DevList nl[2];
    memset(nl, 0, sizeof nl);
    DevList *cur = NULL;
    int in_audio = 0, seen = 0;
    const char *p = txt;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p += len; if (*p == '\n') p++;

        if (!strncmp(line, "Audio", 5)) { in_audio = 1; seen = 1; cur = NULL; continue; }
        if (line[0] && line[0] != ' ' && !(line[0] & 0x80)) { in_audio = 0; cur = NULL; continue; }
        if (!in_audio) continue;
        if (strstr(line, "Sinks:")) cur = &nl[0];
        else if (strstr(line, "Sources:")) cur = &nl[1];
        else if (strstr(line, "Devices:") || strstr(line, "Streams:") ||
                 strstr(line, "endpoints:") || strstr(line, "Filters:")) cur = NULL;
        else if (cur) parse_dev_line(cur, line);
    }
    if (seen) memcpy(lists, nl, sizeof lists);
    return seen;
}

/* ------------------------------------------------------------------ */
/* subprocesses (all non-blocking)                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    pid_t pid; int fd; int active; int capture; int overflow;
    char *buf; size_t len, cap;
    long long t0; unsigned gen;
} Job;

#define MAXBUF (256 * 1024)
static Job j_status, j_vol[2], j_act, j_sub;

static int job_start(Job *j, char *const argv[], int capture)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return -1;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        child_setup(parent, 1);
        setenv("LC_ALL", "C", 1);   /* stable, parseable wpctl/pactl output; helpers only */
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); if (dn > 2) close(dn); }
        dup2(p[1], 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    j->pid = pid; j->fd = p[0]; j->active = 1; j->capture = capture;
    j->len = 0; j->overflow = 0; j->t0 = now_ms();
    return 0;
}

/* returns 1 on EOF / error */
static int job_pump(Job *j, void (*line_cb)(const char *))
{
    char tmp[4096];
    for (;;) {
        ssize_t n = read(j->fd, tmp, sizeof tmp);
        if (n > 0) {
            if ((j->capture || line_cb) && j->len + (size_t)n < MAXBUF) {
                if (j->len + n + 1 > j->cap) {
                    size_t ncap = (j->len + n + 1) * 2;
                    char *nb = realloc(j->buf, ncap);
                    if (!nb) { j->overflow = 1; j->len = 0; return 1; }   /* old buffer stays owned by j */
                    j->buf = nb; j->cap = ncap;
                }
                memcpy(j->buf + j->len, tmp, n);
                j->len += n;
                j->buf[j->len] = 0;
                if (line_cb) {
                    char *s = j->buf, *nl;
                    while ((nl = strchr(s, '\n'))) { *nl = 0; line_cb(s); s = nl + 1; }
                    j->len = strlen(s);
                    memmove(j->buf, s, j->len + 1);
                }
            } else if (line_cb) {
                j->len = 0;
            } else if (j->capture) {
                j->overflow = 1;        /* never parse a truncated status dump */
            }
            continue;
        }
        if (n == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) return 0;
        return 1;
    }
}

/* closes the job, returns 1 if the child exited with status 0 */
static int job_finish(Job *j)
{
    int st = 0, ok;
    if (j->fd >= 0) close(j->fd);
    j->fd = -1; j->active = 0;
    ok = waitpid(j->pid, &st, 0) == j->pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    return ok;
}

static void job_kill(Job *j)
{
    if (j->active) kill(j->pid, SIGKILL);
}

/* fire and forget, double fork so we never leave zombies */
static void spawn_detached(const char *cmd)
{
    pid_t parent = getpid();
    pid_t p = fork();
    if (p == 0) {
        setsid();
        if (fork() == 0) {
            child_setup(parent, 0);
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

/* queued one-shot wpctl actions: set-mute / set-default */
#define AQ 16
static char aq[AQ][3][40];
static int aq_head, aq_n;

static void queue_act(const char *a, const char *b, const char *c)
{
    if (aq_n >= AQ) return;
    int i = (aq_head + aq_n++) % AQ;
    snprintf(aq[i][0], 40, "%s", a);
    snprintf(aq[i][1], 40, "%s", b);
    snprintf(aq[i][2], 40, "%s", c ? c : "");
}

/* ------------------------------------------------------------------ */
/* state, timers                                                       */
/* ------------------------------------------------------------------ */
static int dirty[2]; static double target[2]; static int devid[2];
static long long dirty_t[2];
static long long want_query_at = 0;     /* -1 = none */
static long long sub_retry = 0;

static void request_query(long long delay)
{
    long long w = now_ms() + delay;
    if (want_query_at < 0 || w < want_query_at) want_query_at = w;
}

static int busy(void)
{
    return dirty[0] || dirty[1] || j_vol[0].active || j_vol[1].active ||
           j_act.active || aq_n > 0;
}

/* flash message on the tile */
static char flash[96];
static int flash_w;
static long long flash_t0, flash_until;

/* ------------------------------------------------------------------ */
/* X                                                                    */
/* ------------------------------------------------------------------ */
static Display *dpy;
static int scr, depth;
static Window root, win, iconwin, tilewin;
static GC gc, pgc;
static Colormap cmap;
static Visual *vis;
static Pixmap tile_pm;
static Atom wm_delete;
static int popup_open;
static void redraw(void);
static void popup_close(void);
static void popup_refresh(void);

static unsigned long px(unsigned rgb)
{
    XColor c;
    c.red = ((rgb >> 16) & 255) * 257;
    c.green = ((rgb >> 8) & 255) * 257;
    c.blue = (rgb & 255) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &c);
    return c.pixel;
}

/* ---------------- tile palette ---------------- */
/* Classic dockapp look: black LCD boxes, light-sea-green digits (same as wmclock/wmcpu).
 * Everything outside the boxes is cut away with a shape mask so Window Maker's own
 * dock-tile texture shows through and follows whatever theme is active. */
enum { C_TILE, C_DK, C_LT, C_LCD, C_GREEN, C_GDIM, C_AMBER, C_ADIM, C_RED, C_N };
static unsigned tile_rgb[C_N] = {
    0xa07a4e /* only visible with -w / -D */, 0x202020, 0x606060, 0x000000,
    0x20b2aa, 0x105955, 0xffb52e, 0x5a3f0f, 0xff4a3a };
static const int boxes[3][4] = { { 4, 4, 22, 13 }, { 28, 4, 32, 13 }, { 4, 20, 56, 40 } };
static unsigned long col[C_N];

static void fillr(int x, int y, int w, int h, int c)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(dpy, gc, col[c]);
    XFillRectangle(dpy, tile_pm, gc, x, y, w, h);
}

/* ---------------- 3x5 pixel font ---------------- */
#define G(a, b, c, d, e) ((a << 12) | (b << 9) | (c << 6) | (d << 3) | e)
static int glyph(char c)
{
    switch (c) {
    case '0': return G(7,5,5,5,7); case '1': return G(2,6,2,2,7);
    case '2': return G(7,1,7,4,7); case '3': return G(7,1,7,1,7);
    case '4': return G(5,5,7,1,1); case '5': return G(7,4,7,1,7);
    case '6': return G(7,4,7,5,7); case '7': return G(7,1,2,2,2);
    case '8': return G(7,5,7,5,7); case '9': return G(7,5,7,1,7);
    case 'A': return G(2,5,7,5,5); case 'B': return G(6,5,6,5,6);
    case 'C': return G(3,4,4,4,3); case 'D': return G(6,5,5,5,6);
    case 'E': return G(7,4,6,4,7); case 'F': return G(7,4,6,4,4);
    case 'G': return G(3,4,5,5,3); case 'H': return G(5,5,7,5,5);
    case 'I': return G(7,2,2,2,7); case 'J': return G(1,1,1,5,2);
    case 'K': return G(5,5,6,5,5); case 'L': return G(4,4,4,4,7);
    case 'M': return G(5,7,7,5,5); case 'N': return G(7,5,5,5,5);
    case 'O': return G(2,5,5,5,2); case 'P': return G(6,5,6,4,4);
    case 'Q': return G(2,5,5,7,3); case 'R': return G(6,5,6,5,5);
    case 'S': return G(3,4,2,1,6); case 'T': return G(7,2,2,2,2);
    case 'U': return G(5,5,5,5,7); case 'V': return G(5,5,5,5,2);
    case 'W': return G(5,5,7,7,5); case 'X': return G(5,5,2,5,5);
    case 'Y': return G(5,5,2,2,2); case 'Z': return G(7,1,2,4,7);
    case '%': return G(5,1,2,4,5); case '-': return G(0,0,7,0,0);
    case '.': return G(0,0,0,0,2); case '/': return G(1,1,2,4,4);
    case ':': return G(0,2,0,2,0); case '?': return G(7,1,2,0,2);
    case '+': return G(0,2,7,2,0); case '!': return G(2,2,2,0,2);
    default: return 0;
    }
}

static void draw_glyph(int x, int y, int s, int g, int c, int cx0, int cx1)
{
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 3; k++)
            if ((g >> ((4 - r) * 3 + (2 - k))) & 1) {
                int gx = x + k * s;
                if (gx >= cx0 && gx + s <= cx1) fillr(gx, y + r * s, s, s, c);
            }
}

static void draw_text(int x, int y, int s, const char *t, int c, int cx0, int cx1)
{
    for (; *t; t++, x += 4 * s) draw_glyph(x, y, s, glyph(*t), c, cx0, cx1);
}

static int text_w(const char *t, int s) { int n = (int)strlen(t); return n ? n * 4 * s - s : 0; }

/* ---------------- tile drawing ---------------- */
static void tile_speaker(int x, int y, int lvl, int muted, int c)
{
    fillr(x, y + 4, 3, 3, c);
    fillr(x + 3, y + 3, 1, 5, c);
    fillr(x + 4, y + 2, 1, 7, c);
    fillr(x + 5, y + 1, 1, 9, c);
    if (muted) {
        for (int i = 0; i < 5; i++) { fillr(x + 7 + i, y + 3 + i, 1, 1, c); fillr(x + 11 - i, y + 3 + i, 1, 1, c); }
    } else {
        if (lvl >= 1) fillr(x + 7, y + 4, 1, 3, c);
        if (lvl >= 2) fillr(x + 9, y + 2, 1, 7, c);
    }
}

static void ascii_tile(const char *in, char *out, size_t n)
{
    size_t o = 0; int sp = 1;
    for (; *in && o + 1 < n; in++) {
        unsigned char ch = (unsigned char)*in;
        char c = (ch < 128 && (isalnum(ch) || strchr("-.:/?!+%", ch))) ? (char)toupper(ch) : ' ';
        if (c == ' ') { if (sp) continue; sp = 1; } else sp = 0;
        out[o++] = c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static void set_flash(const char *name)
{
    ascii_tile(name, flash, sizeof flash);
    flash_w = text_w(flash, 1);
    flash_t0 = now_ms();
    flash_until = flash_w <= 49 ? flash_t0 + 1800
                                : flash_t0 + 500 + (long long)(flash_w - 49) * 30 + 800;
}

static void lcd_box(const int *b)
{
    fillr(b[0], b[1], b[2], b[3], C_LT);
    fillr(b[0], b[1], b[2] - 1, b[3] - 1, C_DK);
    fillr(b[0] + 1, b[1] + 1, b[2] - 2, b[3] - 2, C_LCD);
}

static void draw_tile(void)
{
    const Dev *o = defdev(0), *m = defdev(1);
    int pct = o ? (int)lround(o->vol * 100.0) : 0;
    int muted = o && o->muted;
    int ok = pw_ok && o;
    int bright = !ok || muted ? C_AMBER : (pct > 100 ? C_RED : C_GREEN);
    int dim = !ok || muted ? C_ADIM : C_GDIM;

    fillr(0, 0, TILE, TILE, C_TILE);
    for (int i = 0; i < 3; i++) lcd_box(boxes[i]);

    /* box 1: speaker, box 2: label */
    tile_speaker(9, 5, pct == 0 ? 0 : pct < 34 ? 1 : 2, muted || !ok, bright);
    const char *label = !pw_ok ? "NO PW" : !o ? "NO OUT" : muted ? "MUTE" : "VOL";
    draw_text(28 + (32 - text_w(label, 1)) / 2, 8, 1, label, muted || !ok ? C_AMBER : C_GREEN, 0, TILE);

    /* box 3: big number, bar, mic line */
    char num[8];
    if (ok) snprintf(num, sizeof num, "%d", pct); else snprintf(num, sizeof num, "--");
    int n = (int)strlen(num);
    int dw = n * 12 - 3, total = dw + (ok ? 6 : 0);
    int x0 = 4 + (56 - total) / 2;
    draw_text(x0, 24, 3, num, bright, 0, TILE);
    if (ok) draw_glyph(x0 + dw + 3, 34, 1, glyph('%'), bright, 0, TILE);

    int fillpx = ok ? (int)lround(49.0 * pct / (double)cfg.maxpct) : 0;
    if (fillpx > 49) fillpx = 49;
    for (int i = 0; i < 10; i++) {
        int x = 7 + 5 * i, lw = fillpx - 5 * i;
        fillr(x, 43, 4, 6, dim);
        if (lw > 4) lw = 4;
        if (lw > 0) fillr(x, 43, lw, 6, bright);
    }

    long long now = now_ms();
    if (flash_until > now) {
        long long el = now - flash_t0;
        int off = 0;
        if (flash_w > 49) { off = (int)((el - 500) / 30); if (off < 0) off = 0; if (off > flash_w - 49) off = flash_w - 49; }
        draw_text(8 - off, 52, 1, flash, C_GREEN, 8, 57);
    } else {
        int mc = !m ? C_GDIM : m->muted ? C_AMBER : C_GREEN;
        draw_text(8, 52, 1, "MIC", mc, 0, TILE);
        if (m && m->muted) draw_text(24, 52, 1, "OFF", C_AMBER, 0, TILE);
        else if (m) { char mp[8]; snprintf(mp, sizeof mp, "%d", (int)lround(m->vol * 100)); draw_text(24, 52, 1, mp, C_GDIM, 0, TILE); }
    }
}

static void present_tile(void)
{
    XCopyArea(dpy, tile_pm, tilewin, gc, 0, 0, TILE, TILE, 0, 0);
    if (!cfg.windowed) XCopyArea(dpy, tile_pm, win, gc, 0, 0, TILE, TILE, 0, 0);
}

/* ------------------------------------------------------------------ */
/* actions                                                              */
/* ------------------------------------------------------------------ */
static void touch(void) { gen++; }

static void set_volume(int sec, double v)
{
    Dev *d = defdev(sec);
    if (!d) return;
    double mx = cfg.maxpct / 100.0;
    if (v < 0) v = 0;
    if (v > mx) v = mx;
    v = round(v * 100.0) / 100.0;
    d->vol = v;
    target[sec] = v; devid[sec] = d->id;
    dirty[sec] = 1; dirty_t[sec] = now_ms();
    if (d->muted && v > 0) {
        char id[16]; snprintf(id, sizeof id, "%d", d->id);
        queue_act("set-mute", id, "0");
        d->muted = 0;
    }
    touch();
    redraw();
}

static void adjust_volume(int sec, int pct)
{
    const Dev *d = defdev(sec);
    if (!d) return;
    set_volume(sec, round(d->vol * 100.0) / 100.0 + pct / 100.0);
}

static void toggle_mute(int sec)
{
    Dev *d = defdev(sec);
    if (!d) return;
    d->muted = !d->muted;
    char id[16]; snprintf(id, sizeof id, "%d", d->id);
    queue_act("set-mute", id, d->muted ? "1" : "0");
    touch();
    redraw();
}

static void select_device(int sec, int idx)
{
    DevList *l = &lists[sec];
    if (idx < 0 || idx >= l->n || l->d[idx].def) return;
    for (int i = 0; i < l->n; i++) l->d[i].def = (i == idx);
    char id[16]; snprintf(id, sizeof id, "%d", l->d[idx].id);
    queue_act("set-default", id, NULL);
    set_flash(l->d[idx].name);
    touch();
    redraw();
}

static void next_output(void)
{
    const DevList *l = &lists[0];
    if (l->n < 2) return;
    int cur = 0;
    for (int i = 0; i < l->n; i++) if (l->d[i].def) cur = i;
    select_device(0, (cur + 1) % l->n);
}

/* ------------------------------------------------------------------ */
/* popup                                                                */
/* ------------------------------------------------------------------ */
#define PW 320
#define PAD 14
enum { H_MUTE = 1, H_SLIDER, H_DEV, H_MIXER };
typedef struct { int k, sec, idx, x, y, w, h; } Hit;
static Hit hits[64];
_Static_assert(sizeof hits / sizeof hits[0] >= 2 * (MAXDEV + 2) + 1, "hits[] too small for MAXDEV");
static int nhits, hover = -1, drag_sec = -1;
static Window pop;
static Pixmap pop_pm;
static XftDraw *pop_xd;
static XftFont *font;
static int pop_h, fh, rh, sec_ty[2], sec_split, empty_y[2];
static int pop_x, pop_y;

typedef struct { unsigned long px; XftColor x; } PC;
enum { P_BG, P_BORDER, P_TEXT, P_DIM, P_HOVER, P_ACC, P_TRACK, P_AMB, P_N };
static const char *pc_name[P_N] = {
    "#1f2329", "#3a424c", "#e6eaf0", "#8b95a3", "#2c323a", "#4cd98a", "#39414b", "#ffb52e" };
static PC pc[P_N];
static int pc_ready;

static void popup_colors(void)
{
    if (pc_ready) return;
    static char acc[8];
    snprintf(acc, sizeof acc, "#%06x", cfg.fg);
    pc_name[P_ACC] = acc;
    for (int i = 0; i < P_N; i++) {
        XftColorAllocName(dpy, vis, cmap, pc_name[i], &pc[i].x);
        pc[i].px = pc[i].x.pixel;
    }
    pc_ready = 1;
}

static void add_hit(int k, int sec, int idx, int x, int y, int w, int h)
{
    if (nhits >= (int)(sizeof hits / sizeof hits[0])) return;
    hits[nhits++] = (Hit){ k, sec, idx, x, y, w, h };
}

static void popup_layout(void)
{
    fh = font->ascent + font->descent;
    rh = fh + 12 < 30 ? 30 : fh + 12;
    nhits = 0;
    int y = PAD;
    for (int sec = 0; sec < 2; sec++) {
        if (sec == 1) sec_split = y - 4;
        sec_ty[sec] = y;
        y += fh + 8;
        add_hit(H_MUTE, sec, 0, PAD - 4, y, rh, rh);
        add_hit(H_SLIDER, sec, 0, PAD + rh + 8, y, PW - 2 * PAD - rh - 8 - 48, rh);
        y += rh + 6;
        const DevList *l = &lists[sec];
        if (l->n == 0) { empty_y[sec] = y; y += rh; }
        for (int i = 0; i < l->n; i++) { add_hit(H_DEV, sec, i, PAD / 2, y, PW - PAD, rh); y += rh; }
        y += 10;
    }
    add_hit(H_MIXER, 0, 0, PAD / 2, y + 2, PW - PAD, rh);
    y += rh + 2 + PAD / 2;
    pop_h = y;
}

static void p_fill(int x, int y, int w, int h, int c)
{
    XSetForeground(dpy, pgc, pc[c].px);
    XFillRectangle(dpy, pop_pm, pgc, x, y, w, h);
}

static void p_rrect(int x, int y, int w, int h, int r, int c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    XSetForeground(dpy, pgc, pc[c].px);
    XFillRectangle(dpy, pop_pm, pgc, x + r, y, w - 2 * r, h);
    XFillRectangle(dpy, pop_pm, pgc, x, y + r, w, h - 2 * r);
    XFillArc(dpy, pop_pm, pgc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
    XFillArc(dpy, pop_pm, pgc, x + w - 2 * r - 1, y, 2 * r, 2 * r, 0, 90 * 64);
    XFillArc(dpy, pop_pm, pgc, x, y + h - 2 * r - 1, 2 * r, 2 * r, 180 * 64, 90 * 64);
    XFillArc(dpy, pop_pm, pgc, x + w - 2 * r - 1, y + h - 2 * r - 1, 2 * r, 2 * r, 270 * 64, 90 * 64);
}

static void p_circle(int cx, int cy, int r, int c, int filled)
{
    XSetForeground(dpy, pgc, pc[c].px);
    if (filled) XFillArc(dpy, pop_pm, pgc, cx - r, cy - r, 2 * r + 1, 2 * r + 1, 0, 360 * 64);
    else XDrawArc(dpy, pop_pm, pgc, cx - r, cy - r, 2 * r, 2 * r, 0, 360 * 64);
}

static int p_textw(const char *s, int n)
{
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, n, &gi);
    return gi.xOff;
}

static void p_text(int x, int base, const char *s, int c)
{
    XftDrawStringUtf8(pop_xd, &pc[c].x, font, x, base, (const FcChar8 *)s, (int)strlen(s));
}

static void p_text_ellip(int x, int base, int maxw, const char *s, int c)
{
    int n = (int)strlen(s);
    if (p_textw(s, n) <= maxw) { p_text(x, base, s, c); return; }
    char buf[160];
    if (n > 150) n = 150;
    while (n > 0) {
        n--;
        while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
        memcpy(buf, s, n);
        strcpy(buf + n, "\xe2\x80\xa6");
        if (p_textw(buf, (int)strlen(buf)) <= maxw) break;
    }
    p_text(x, base, buf, c);
}

static void p_line(int x1, int y1, int x2, int y2, int c, int w)
{
    XSetForeground(dpy, pgc, pc[c].px);
    XSetLineAttributes(dpy, pgc, w, LineSolid, CapRound, JoinRound);
    XDrawLine(dpy, pop_pm, pgc, x1, y1, x2, y2);
}

static void icon_speaker(int cx, int cy, int u, int c, int lvl, int muted)
{
    XSetForeground(dpy, pgc, pc[c].px);
    XFillRectangle(dpy, pop_pm, pgc, cx - 4 * u, cy - 2 * u, 2 * u, 4 * u);
    XPoint p[4] = { { cx - 2 * u, cy - 2 * u }, { cx + u, cy - 4 * u }, { cx + u, cy + 4 * u }, { cx - 2 * u, cy + 2 * u } };
    XFillPolygon(dpy, pop_pm, pgc, p, 4, Convex, CoordModeOrigin);
    if (muted) {
        p_line(cx + 3 * u, cy - 2 * u, cx + 7 * u, cy + 2 * u, P_AMB, 2);
        p_line(cx + 7 * u, cy - 2 * u, cx + 3 * u, cy + 2 * u, P_AMB, 2);
        return;
    }
    XSetLineAttributes(dpy, pgc, 2, LineSolid, CapRound, JoinRound);
    if (lvl >= 1) XDrawArc(dpy, pop_pm, pgc, cx + u - 2 * u, cy - 2 * u, 4 * u, 4 * u, -50 * 64, 100 * 64);
    if (lvl >= 2) XDrawArc(dpy, pop_pm, pgc, cx + u - 4 * u, cy - 4 * u, 8 * u, 8 * u, -50 * 64, 100 * 64);
}

static void icon_mic(int cx, int cy, int u, int c, int muted)
{
    XSetForeground(dpy, pgc, pc[c].px);
    XFillRectangle(dpy, pop_pm, pgc, cx - u, cy - 4 * u + u, 2 * u + 1, 4 * u);
    XFillArc(dpy, pop_pm, pgc, cx - u, cy - 5 * u, 2 * u + 1, 2 * u + 1, 0, 180 * 64);
    XFillArc(dpy, pop_pm, pgc, cx - u, cy - 2 * u + u, 2 * u + 1, 2 * u + 1, 180 * 64, 180 * 64);
    XSetLineAttributes(dpy, pgc, 2, LineSolid, CapRound, JoinRound);
    XDrawArc(dpy, pop_pm, pgc, cx - 3 * u, cy - 4 * u, 6 * u, 6 * u, 180 * 64, 180 * 64);
    p_line(cx, cy + 2 * u, cx, cy + 4 * u, c, 2);
    p_line(cx - 2 * u, cy + 4 * u, cx + 2 * u, cy + 4 * u, c, 2);
    if (muted) p_line(cx - 4 * u, cy - 5 * u, cx + 4 * u, cy + 5 * u, P_AMB, 2);
}

static void popup_paint(void)
{
    p_fill(0, 0, PW, pop_h, P_BG);
    XSetForeground(dpy, pgc, pc[P_BORDER].px);
    XSetLineAttributes(dpy, pgc, 1, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(dpy, pop_pm, pgc, 0, 0, PW - 1, pop_h - 1);

    static const char *titles[2] = { "OUTPUT", "INPUT" };
    for (int s = 0; s < 2; s++) {
        p_text(PAD, sec_ty[s] + font->ascent, titles[s], P_DIM);
        if (s == 1) {
            XSetForeground(dpy, pgc, pc[P_BORDER].px);
            XDrawLine(dpy, pop_pm, pgc, PAD, sec_split - 2, PW - PAD, sec_split - 2);
        }
        if (lists[s].n == 0)
            p_text(PAD + 8, empty_y[s] + (rh + font->ascent - font->descent) / 2,
                   !pw_ok ? "wpctl not available" : s ? "No input devices" : "No output devices", P_DIM);
    }

    for (int i = 0; i < nhits; i++) {
        const Hit *h = &hits[i];
        if (h->k == H_MIXER) {
            XSetForeground(dpy, pgc, pc[P_BORDER].px);
            XDrawLine(dpy, pop_pm, pgc, PAD, h->y - 4, PW - PAD, h->y - 4);
        }
        const Dev *d = defdev(h->sec);
        int base = h->y + (h->h + font->ascent - font->descent) / 2;
        int hv = (i == hover);
        switch (h->k) {
        case H_MUTE: {
            if (hv) p_rrect(h->x, h->y, h->w, h->h, 6, P_HOVER);
            int muted = !d || d->muted, u = h->h / 12;
            if (u < 2) u = 2;
            int c = muted ? P_DIM : P_TEXT;
            if (h->sec == 0) icon_speaker(h->x + h->w / 2 - u, h->y + h->h / 2, u, c,
                                          !d || d->vol <= 0 ? 0 : d->vol < 0.34 ? 1 : 2, d && d->muted);
            else icon_mic(h->x + h->w / 2, h->y + h->h / 2, u, c, d && d->muted);
            break;
        }
        case H_SLIDER: {
            double mx = cfg.maxpct / 100.0;
            double f = d ? d->vol / mx : 0;
            if (f > 1) f = 1;
            int tx = h->x + 7, tw = h->w - 14, cy = h->y + h->h / 2;
            int ac = (!d || d->muted) ? P_DIM : P_ACC;
            p_rrect(tx, cy - 3, tw, 6, 3, P_TRACK);
            int fw = (int)(tw * f);
            if (fw > 0) p_rrect(tx, cy - 3, fw < 6 ? 6 : fw, 6, 3, ac);
            p_circle(tx + fw, cy, hv || drag_sec == h->sec ? 8 : 7, P_TEXT, 1);
            char t[16];
            if (d) snprintf(t, sizeof t, "%d%%", (int)lround(d->vol * 100)); else snprintf(t, sizeof t, "--");
            int w = p_textw(t, (int)strlen(t));
            p_text(PW - PAD - w, base, t, d && d->muted ? P_AMB : P_TEXT);
            break;
        }
        case H_DEV: {
            Dev *dv = &lists[h->sec].d[h->idx];
            if (hv) p_rrect(h->x, h->y, h->w, h->h, 6, P_HOVER);
            int cy = h->y + h->h / 2, cx = h->x + 18;
            p_circle(cx, cy, 6, dv->def ? P_ACC : P_DIM, 0);
            if (dv->def) p_circle(cx, cy, 3, P_ACC, 1);
            p_text_ellip(h->x + 34, base, h->w - 34 - 10, dv->name, dv->def ? P_TEXT : P_DIM);
            break;
        }
        case H_MIXER: {
            if (hv) p_rrect(h->x, h->y, h->w, h->h, 6, P_HOVER);
            p_text(h->x + 18, base, "Open full mixer", P_TEXT);
            const char *m = cfg.mixer;
            int w = p_textw(m, (int)strlen(m));
            p_text(h->x + h->w - 12 - w, base, m, P_DIM);
            break;
        }
        }
    }
}

static void popup_present(void)
{
    if (!popup_open) return;
    popup_paint();
    XCopyArea(dpy, pop_pm, pop, pgc, 0, 0, PW, pop_h, 0, 0);
}

static int hit_at(int x, int y)
{
    for (int i = 0; i < nhits; i++)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) return i;
    return -1;
}

static void popup_alloc_buffers(void)
{
    if (pop_xd) XftDrawDestroy(pop_xd);
    if (pop_pm) XFreePixmap(dpy, pop_pm);
    pop_pm = XCreatePixmap(dpy, pop, PW, pop_h, depth);
    pop_xd = XftDrawCreate(dpy, pop_pm, vis, cmap);
}

static void popup_open_fn(void)
{
    if (popup_open) return;
    if (!font) font = XftFontOpenName(dpy, scr, cfg.font);
    if (!font) { fprintf(stderr, "wmvol: cannot load font '%s'\n", cfg.font); return; }
    popup_colors();
    popup_layout();

    int tx, ty; Window c;
    XTranslateCoordinates(dpy, tilewin, root, 0, 0, &tx, &ty, &c);
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    int x = (tx + TILE / 2 < sw / 2) ? tx + TILE + 6 : tx - PW - 6;
    int y = ty;
    if (x + PW > sw) x = sw - PW;
    if (x < 0) x = 0;
    if (y + pop_h > sh) y = sh - pop_h;
    if (y < 0) y = 0;
    pop_x = x; pop_y = y;

    XSetWindowAttributes xa;
    xa.override_redirect = True;
    xa.background_pixel = pc[P_BG].px;
    xa.border_pixel = pc[P_BORDER].px;
    xa.save_under = True;
    xa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                    LeaveWindowMask | KeyPressMask;
    pop = XCreateWindow(dpy, root, x, y, PW, pop_h, 0, depth, InputOutput, vis,
                        CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWSaveUnder | CWEventMask, &xa);
    popup_alloc_buffers();
    popup_open = 1;
    hover = -1; drag_sec = -1;
    XMapRaised(dpy, pop);
    XSync(dpy, False);
    if (!cfg.dump) {
        Cursor cur = XCreateFontCursor(dpy, XC_left_ptr);
        int pg = XGrabPointer(dpy, pop, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                              GrabModeAsync, GrabModeAsync, None, cur, CurrentTime);
        int kg = XGrabKeyboard(dpy, pop, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        XFreeCursor(dpy, cur);
        /* Without the pointer grab there is no click-outside-to-close, and the
         * popup would get stuck on screen; without the keyboard grab, Esc is
         * dead. Another client (menu, screensaver) can hold either one. */
        if (pg != GrabSuccess || kg != GrabSuccess) { popup_close(); return; }
    }
    popup_present();
    /* make sure the numbers are fresh while it is open */
    request_query(0);
}

static void popup_close(void)
{
    if (!popup_open) return;
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    if (pop_xd) { XftDrawDestroy(pop_xd); pop_xd = NULL; }
    if (pop_pm) { XFreePixmap(dpy, pop_pm); pop_pm = 0; }
    XDestroyWindow(dpy, pop);
    popup_open = 0; hover = -1; drag_sec = -1;
}

/* device list changed while open: relayout / resize */
static void popup_refresh(void)
{
    if (!popup_open) return;
    int old = pop_h;
    popup_layout();
    hover = -1;     /* hit indices were rebuilt; the next motion event re-arms it */
    if (pop_h != old) {
        int sh = DisplayHeight(dpy, scr);
        if (pop_y + pop_h > sh) pop_y = sh - pop_h < 0 ? 0 : sh - pop_h;
        XMoveResizeWindow(dpy, pop, pop_x, pop_y, PW, pop_h);
        popup_alloc_buffers();
    }
    popup_present();
}

static void redraw(void)
{
    draw_tile();
    present_tile();
    if (popup_open) popup_present();
}

static void slider_set(int sec, int mx)
{
    for (int i = 0; i < nhits; i++) {
        if (hits[i].k == H_SLIDER && hits[i].sec == sec) {
            int tx = hits[i].x + 7, tw = hits[i].w - 14;
            double f = (double)(mx - tx) / tw;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            set_volume(sec, f * cfg.maxpct / 100.0);
            return;
        }
    }
}

static void popup_button(const XButtonEvent *e)
{
    if (e->x < 0 || e->y < 0 || e->x >= PW || e->y >= pop_h) { popup_close(); return; }
    if (e->button == Button4 || e->button == Button5) {
        int sec = e->y >= sec_split ? 1 : 0;
        int st = (e->state & ShiftMask) ? 1 : cfg.step;
        adjust_volume(sec, e->button == Button4 ? st : -st);
        return;
    }
    if (e->button != Button1) return;
    int i = hit_at(e->x, e->y);
    if (i < 0) return;
    const Hit *h = &hits[i];
    switch (h->k) {
    case H_MUTE: toggle_mute(h->sec); break;
    case H_SLIDER: drag_sec = h->sec; slider_set(h->sec, e->x); break;
    case H_DEV: select_device(h->sec, h->idx); break;
    case H_MIXER: spawn_detached(cfg.mixer); popup_close(); break;
    }
}

static void tile_button(const XButtonEvent *e)
{
    int ctrl = (e->state & ControlMask) != 0;
    int st = (e->state & ShiftMask) ? 1 : cfg.step;
    switch (e->button) {
    case Button1: if (popup_open) popup_close(); else popup_open_fn(); break;
    case Button2: toggle_mute(ctrl ? 1 : 0); break;
    case Button3: if (ctrl) spawn_detached(cfg.mixer); else next_output(); break;
    case Button4: adjust_volume(ctrl ? 1 : 0, st); break;
    case Button5: adjust_volume(ctrl ? 1 : 0, -st); break;
    }
}

static void handle_event(void)
{
    XEvent ev;
    XNextEvent(dpy, &ev);
    switch (ev.type) {
    case Expose:
        if (ev.xexpose.count) break;
        if (popup_open && ev.xexpose.window == pop) popup_present();
        else present_tile();
        break;
    case ButtonPress:
        if (popup_open && ev.xbutton.window == pop) popup_button(&ev.xbutton);
        else if (ev.xbutton.window == win || ev.xbutton.window == iconwin) tile_button(&ev.xbutton);
        break;
    case ButtonRelease:
        drag_sec = -1;
        break;
    case MotionNotify:
        if (popup_open && ev.xmotion.window == pop) {
            if (drag_sec >= 0) slider_set(drag_sec, ev.xmotion.x);
            else {
                int h = hit_at(ev.xmotion.x, ev.xmotion.y);
                if (h != hover) { hover = h; popup_present(); }
            }
        }
        break;
    case LeaveNotify:
        if (popup_open && ev.xcrossing.window == pop && hover != -1) { hover = -1; popup_present(); }
        break;
    case KeyPress:
        if (popup_open && XLookupKeysym(&ev.xkey, 0) == XK_Escape) popup_close();
        break;
    case ClientMessage:
        if ((Atom)ev.xclient.data.l[0] == wm_delete) quit_flag = 1;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* window setup                                                         */
/* ------------------------------------------------------------------ */
static void setup_window(int argc, char **argv)
{
    XSizeHints sh;
    XClassHint ch = { (char *)"wmvol", (char *)"WMVol" };
    XWMHints wm;
    char *name = (char *)"wmvol";
    XTextProperty wname;

    memset(&sh, 0, sizeof sh);
    sh.flags = USSize | USPosition | PMinSize | PMaxSize;
    sh.width = sh.min_width = sh.max_width = TILE;
    sh.height = sh.min_height = sh.max_height = TILE;

    unsigned long bg = BlackPixel(dpy, scr);
    win = XCreateSimpleWindow(dpy, root, 0, 0, TILE, TILE, 0, bg, bg);
    XSetWMNormalHints(dpy, win, &sh);
    XSetClassHint(dpy, win, &ch);
    XStringListToTextProperty(&name, 1, &wname);
    XSetWMName(dpy, win, &wname);
    XFree(wname.value);

    long mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;
    XSelectInput(dpy, win, mask);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetGraphicsExposures(dpy, gc, False);

    if (cfg.windowed) {
        iconwin = 0; tilewin = win;
        wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, win, &wm_delete, 1);
    } else {
        iconwin = XCreateSimpleWindow(dpy, win, 0, 0, TILE, TILE, 0, bg, bg);
        XSelectInput(dpy, iconwin, mask);
        tilewin = iconwin;
        memset(&wm, 0, sizeof wm);
        wm.initial_state = WithdrawnState;
        wm.icon_window = iconwin;
        wm.window_group = win;
        wm.flags = StateHint | IconWindowHint | WindowGroupHint;
        XSetWMHints(dpy, win, &wm);
    }
    XSetCommand(dpy, win, argv, argc);

    /* cut the tile down to the LCD boxes so the WM theme shows around them */
    int ev, er;
    if (!cfg.windowed && XShapeQueryExtension(dpy, &ev, &er)) {
        Pixmap shape = XCreatePixmap(dpy, root, TILE, TILE, 1);
        GC mg = XCreateGC(dpy, shape, 0, NULL);
        XSetForeground(dpy, mg, 0);
        XFillRectangle(dpy, shape, mg, 0, 0, TILE, TILE);
        XSetForeground(dpy, mg, 1);
        for (int i = 0; i < 3; i++) XFillRectangle(dpy, shape, mg, boxes[i][0], boxes[i][1], boxes[i][2], boxes[i][3]);
        XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, shape, ShapeSet);
        XShapeCombineMask(dpy, iconwin, ShapeBounding, 0, 0, shape, ShapeSet);
        XFreeGC(dpy, mg);
        XFreePixmap(dpy, shape);
    }
    XMapWindow(dpy, win);
}

/* ------------------------------------------------------------------ */
/* debug screenshots                                                    */
/* ------------------------------------------------------------------ */
static void dump_ppm(Drawable d, int w, int h, const char *path)
{
    XImage *im = XGetImage(dpy, d, 0, 0, w, h, AllPlanes, ZPixmap);
    FILE *f = fopen(path, "wb");
    if (!im || !f) { if (im) XDestroyImage(im); if (f) fclose(f); return; }
    unsigned long rm = vis->red_mask, gm = vis->green_mask, bm = vis->blue_mask;
    int rs = __builtin_ctzl(rm), gs = __builtin_ctzl(gm), bs = __builtin_ctzl(bm);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned long p = XGetPixel(im, x, y);
            fputc((p & rm) >> rs, f); fputc((p & gm) >> gs, f); fputc((p & bm) >> bs, f);
        }
    fclose(f);
    XDestroyImage(im);
}

static void do_dump(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s-tile.ppm", cfg.dump);
    dump_ppm(tile_pm, TILE, TILE, path);
    popup_open_fn();
    if (popup_open) {
        popup_present();
        snprintf(path, sizeof path, "%s-popup.ppm", cfg.dump);
        dump_ppm(pop_pm, PW, pop_h, path);
    }
    quit_flag = 1;
}

/* ------------------------------------------------------------------ */
/* pactl subscribe -> refresh                                           */
/* ------------------------------------------------------------------ */
static void sub_line(const char *l)
{
    if (strstr(l, "sink #") || strstr(l, "source #") || strstr(l, "server") || strstr(l, "card #"))
        request_query(120);
}

/* ------------------------------------------------------------------ */
/* main loop                                                            */
/* ------------------------------------------------------------------ */
static void usage(const char *p)
{
    printf("wmvol %s - Window Maker PipeWire volume dockapp\n\n"
           "usage: %s [options]\n"
           "  -display DPY   X display\n"
           "  -w             run in a normal window instead of docking\n"
           "  -s N           wheel step in percent (default 5)\n"
           "  -l N           volume limit in percent (default 100)\n"
           "  -m CMD         mixer command (default pavucontrol)\n"
           "  -f FONT        popup font, Xft pattern (default sans-10)\n"
           "  -fg RRGGBB     LCD colour (default 20b2aa, same as wmclock/wmcpu)\n"
           "  -D PREFIX      debug: write PREFIX-tile.ppm / -popup.ppm and exit\n"
           "  -h, -v         help / version\n\n"
           "tile:  left click  open popup        wheel         output volume\n"
           "       middle      mute output       ctrl+wheel    input volume\n"
           "       right       next output       ctrl+middle   mute input\n"
           "       ctrl+right  open mixer        shift+wheel   1%% steps\n", VERSION, p);
}

static void start_status(void)
{
    char *argv[] = { (char *)"wpctl", (char *)"status", NULL };
    j_status.gen = gen;
    if (job_start(&j_status, argv, 1) < 0) { pw_ok = 0; want_query_at = now_ms() + 5000; }
    else want_query_at = -1;
}

static void run_timers(long long now)
{
    if (flash_until) {
        if (now >= flash_until) { flash_until = 0; redraw(); }
        else redraw();
    }
    for (int k = 0; k < 2; k++) {
        if (dirty[k] && !j_vol[k].active && now - dirty_t[k] >= 20) {
            char id[16], v[16], lim[16];
            snprintf(id, sizeof id, "%d", devid[k]);
            snprintf(v, sizeof v, "%.2f", target[k]);
            snprintf(lim, sizeof lim, "%.2f", cfg.maxpct / 100.0);
            char *argv[] = { (char *)"wpctl", (char *)"set-volume", (char *)"-l", lim, id, v, NULL };
            dirty[k] = 0;
            if (job_start(&j_vol[k], argv, 0) < 0) request_query(100);
        }
    }
    if (!j_act.active && aq_n > 0) {
        char *argv[5] = { (char *)"wpctl", aq[aq_head][0], aq[aq_head][1], NULL, NULL };
        if (aq[aq_head][2][0]) argv[3] = aq[aq_head][2];
        if (job_start(&j_act, argv, 0) < 0) request_query(100);
        aq_head = (aq_head + 1) % AQ; aq_n--;
    }
    if (!j_status.active && !busy() && want_query_at >= 0 && now >= want_query_at) start_status();
    if (!j_sub.active && now >= sub_retry) {
        char *argv[] = { (char *)"pactl", (char *)"subscribe", NULL };
        if (job_start(&j_sub, argv, 0) < 0) sub_retry = now + 30000;
    }
    Job *all[] = { &j_status, &j_vol[0], &j_vol[1], &j_act };
    for (int i = 0; i < 4; i++)
        if (all[i]->active && now - all[i]->t0 > 4000) job_kill(all[i]);
}

static void service(struct pollfd *p, Job *j, int kind)
{
    if (!j->active || p->fd != j->fd || !(p->revents & (POLLIN | POLLHUP | POLLERR))) return;
    int done = job_pump(j, kind == 4 ? sub_line : NULL);
    if (!done) return;
    int ok = job_finish(j);
    switch (kind) {
    case 0: {   /* status */
        if (j->gen != gen) {            /* user changed something meanwhile: discard, ask again */
            request_query(60);
            break;
        }
        pw_ok = ok && !j->overflow && j->buf && j->len > 0 && parse_status(j->buf);
        request_query(pw_ok ? (j_sub.active ? 15000 : 2000) : 5000);
        redraw();
        popup_refresh();
        if (cfg.dump) do_dump();
        break;
    }
    case 1: case 2: if (!dirty[kind - 1]) request_query(80); break;
    case 3: request_query(60); break;
    case 4: sub_retry = now_ms() + 30000; request_query(0); break;
    }
}

/* option value helpers: fail loudly instead of silently turning typos into 0 */
static const char *opt_arg(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "wmvol: option %s needs an argument\n", argv[*i]);
        usage(argv[0]);
        exit(1);
    }
    return argv[++*i];
}

static long opt_num(const char *opt, const char *v, int base, long lo, long hi)
{
    char *end;
    errno = 0;
    long n = strtol(v, &end, base);
    if (errno || end == v || *end || n < lo || n > hi) {
        fprintf(stderr, "wmvol: bad value '%s' for %s\n", v, opt);
        exit(1);
    }
    return n;
}

/* X connection lost (Window Maker quit/restarted): Xlib would just exit(),
 * leaving helpers behind. Take them down first. Must not return. */
static int on_x_io_error(Display *d)
{
    (void)d;
    if (j_sub.active) kill(j_sub.pid, SIGKILL);
    _exit(1);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-display") || !strcmp(a, "-d")) cfg.display = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-w")) cfg.windowed = 1;
        else if (!strcmp(a, "-s")) cfg.step = (int)opt_num(a, opt_arg(argc, argv, &i), 10, 1, 100);
        else if (!strcmp(a, "-l")) cfg.maxpct = (int)opt_num(a, opt_arg(argc, argv, &i), 10, 10, 200);
        else if (!strcmp(a, "-m")) cfg.mixer = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-f")) cfg.font = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-D")) cfg.dump = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-fg")) {
            const char *h = opt_arg(argc, argv, &i);
            if (*h == '#') h++;
            cfg.fg = (unsigned)opt_num(a, h, 16, 0, 0xffffff);
        }
        else if (!strcmp(a, "-v")) { printf("wmvol %s\n", VERSION); return 0; }
        else { usage(argv[0]); return !strcmp(a, "-h") ? 0 : 1; }
    }

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);

    XSetIOErrorHandler(on_x_io_error);
    dpy = XOpenDisplay(cfg.display);
    if (!dpy) { fprintf(stderr, "wmvol: cannot open display\n"); return 1; }
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    depth = DefaultDepth(dpy, scr);
    vis = DefaultVisual(dpy, scr);
    cmap = DefaultColormap(dpy, scr);

    setup_window(argc, argv);
    pgc = XCreateGC(dpy, root, 0, NULL);
    XSetGraphicsExposures(dpy, pgc, False);
    tile_rgb[C_GREEN] = cfg.fg;
    tile_rgb[C_GDIM] = ((cfg.fg >> 1) & 0x7f7f7f);
    for (int i = 0; i < C_N; i++) col[i] = px(tile_rgb[i]);
    tile_pm = XCreatePixmap(dpy, root, TILE, TILE, depth);
    j_status.fd = j_vol[0].fd = j_vol[1].fd = j_act.fd = j_sub.fd = -1;
    redraw();

    int xfd = ConnectionNumber(dpy);
    while (!quit_flag) {
        while (XPending(dpy)) handle_event();
        if (quit_flag) break;
        long long now = now_ms();
        run_timers(now);
        XFlush(dpy);

        long long to = 1000;
        #define MINTO(t) do { long long _d = (t) - now; if (_d < 0) _d = 0; if (_d < to) to = _d; } while (0)
        if (flash_until) to = 30;
        for (int k = 0; k < 2; k++) if (dirty[k]) MINTO(dirty_t[k] + 20);
        if (want_query_at >= 0 && !j_status.active) MINTO(want_query_at);
        if (!j_sub.active) MINTO(sub_retry);
        if (j_status.active || j_vol[0].active || j_vol[1].active || j_act.active) { if (to > 250) to = 250; }

        struct pollfd pf[6];
        Job *js[5] = { &j_status, &j_vol[0], &j_vol[1], &j_act, &j_sub };
        int n = 0;
        pf[n].fd = xfd; pf[n].events = POLLIN; pf[n].revents = 0; n++;
        for (int i = 0; i < 5; i++) {
            pf[n].fd = js[i]->active ? js[i]->fd : -1;
            pf[n].events = POLLIN; pf[n].revents = 0; n++;
        }
        poll(pf, n, (int)to);
        for (int i = 0; i < 5; i++) service(&pf[i + 1], js[i], i);
    }

    popup_close();
    job_kill(&j_sub);
    return 0;
}
