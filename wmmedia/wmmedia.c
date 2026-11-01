/* SPDX-License-Identifier: MIT */
/*
 * wmmedia - Window Maker dockapp: media player controls
 *
 * A 64x64 dock tile in the same LCD style as wmvol: three black bevelled
 * boxes, teal digits, shaped so Window Maker's own tile texture frames them.
 *
 *   +------+ +--------+     state icon, elapsed / remaining time
 *   +------+ +--------+
 *   +------------------+
 *   | TITLE (scrolls)  |     title and artist tickers
 *   | ARTIST           |
 *   | [|<<] [>] [>>|]  |     previous / play-pause / next
 *   | ##########----   |     progress bar (click or drag to seek)
 *   +------------------+
 *
 * Mouse:  left click on a key        previous / play-pause / next
 *         left click or drag on bar  seek
 *         left click on the time     elapsed <-> remaining
 *         wheel                      volume (shift: 1% steps)
 *         middle click               stop
 *
 * Plain C99: the only dependencies are libX11 and libXext.  Everything else is
 * POSIX: fork/exec/pipe/poll/waitpid, no Linux-only calls.
 *
 * Player backends are external commands (playerctl for MPRIS players, or mpc
 * for MPD).  They are run as short non-blocking subprocesses polled from the
 * main loop, so the UI never waits on a player.  Between polls the clock is
 * extrapolated locally, so the time and bar move smoothly.
 *
 * Window Maker plumbing (withdrawn state, icon window, XShape cut-out, 3x5
 * pixel font, LCD look) follows wmvol, MIT, see LICENSE.
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION "2.1.0"
#define TILE 64

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */
static struct {
    const char *display;
    const char *backend;    /* "playerctl" | "mpc" */
    const char *player;     /* playerctl -p NAME */
    int windowed;
    int step;               /* wheel volume step, percent */
    unsigned fg;            /* LCD colour, 0xRRGGBB */
    const char *dump;       /* debug: write PPM screenshot and exit */
    int use_theme;          /* derive the LCD colour from the Window Maker theme */
    const char *conf;       /* Window Maker defaults file override */
    int remaining;          /* show remaining instead of elapsed time */
} cfg = { NULL, "playerctl", NULL, 0, 5, 0x20b2aa, NULL, 0, NULL, 0 };

static volatile sig_atomic_t quit_flag;
static void on_sig(int s) { (void)s; quit_flag = 1; }

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}


/* ------------------------------------------------------------------ */
/* media state + parsers (plain C so they can be unit tested)          */
/* ------------------------------------------------------------------ */
typedef enum { PS_NONE, PS_STOPPED, PS_PAUSED, PS_PLAYING } PlayState;
#define FIELD 96
#define MAX_SEC 359999          /* 99:59:59: keeps every clock inside the LCD */
typedef struct {
    PlayState ps;
    int vol;                /* percent, -1 unknown */
    int pos, len;           /* seconds, -1 unknown */
    char artist[FIELD], title[FIELD];
} MediaState;

static void state_clear(MediaState *s) { memset(s, 0, sizeof *s); s->vol = s->pos = s->len = -1; }

static void copy_n(char *dst, size_t n, const char *s, size_t len)
{
    if (n == 0) return;
    if (len >= n) len = n - 1;
    memcpy(dst, s, len);
    dst[len] = 0;
}

static void copy_clip(char *dst, size_t n, const char *s) { copy_n(dst, n, s, strlen(s)); }

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* plain decimal microseconds -> whole seconds; -1 if empty or not a number */
static int usec_to_sec(const char *s)
{
    long long v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        if (v < 1000000000000LL) v = v * 10 + (*s - '0');
    }
    v /= 1000000;
    return v > MAX_SEC ? MAX_SEC : (int)v;
}

/* is [s, end) all decimal digits (or empty)? */
static int digits_or_empty(const char *s, const char *end)
{
    for (; s < end; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* position/length sanity: unknown length (0, streams) is -1, position never past the end */
static void fix_times(MediaState *st)
{
    if (st->len <= 0) st->len = -1;
    if (st->pos >= 0 && st->len > 0 && st->pos > st->len) st->pos = st->len;
}

/* playerctl metadata --format
 *   '{{status}}\t{{volume}}\t{{artist}}\t{{title}}\t{{position}}\t{{mpris:length}}'
 * Position and length are microseconds.  The older four-field form (no times) is
 * accepted too, and a title that itself contains tabs survives.
 * Returns 1 if the line looks like player output. */
static int parse_playerctl(const char *out, MediaState *st)
{
    char line[1024];
    const char *nl = strchr(out, '\n');
    size_t len = nl ? (size_t)(nl - out) : strlen(out);
    copy_n(line, sizeof line, out, len);

    char *first[3] = { NULL, NULL, NULL }, *last1 = NULL, *last2 = NULL;
    int nt = 0;
    for (char *p = strchr(line, '\t'); p; p = strchr(p + 1, '\t')) {
        if (nt < 3) first[nt] = p;
        last2 = last1; last1 = p; nt++;
    }
    if (nt < 3) return 0;

    char *status = line, *vol = first[0] + 1, *artist = first[1] + 1, *title = first[2] + 1;
    char *pos = NULL, *plen = NULL;
    if (nt >= 5 && last2 > first[2] && digits_or_empty(last2 + 1, last1) &&
        digits_or_empty(last1 + 1, last1 + 1 + strlen(last1 + 1))) {
        pos = last2 + 1; plen = last1 + 1;
        *last2 = 0; *last1 = 0;
    }
    *first[0] = 0; *first[1] = 0; *first[2] = 0;

    PlayState ps;
    if (!strcasecmp(status, "playing")) ps = PS_PLAYING;
    else if (!strcasecmp(status, "paused")) ps = PS_PAUSED;
    else if (!strcasecmp(status, "stopped")) ps = PS_STOPPED;
    else return 0;

    state_clear(st);
    st->ps = ps;
    if (*vol) {
        char *end;
        double v = strtod(vol, &end);
        if (end != vol && v >= 0 && v < 10) st->vol = (int)(v * 100.0 + 0.5);
    }
    copy_clip(st->artist, sizeof st->artist, artist);
    copy_clip(st->title, sizeof st->title, title);
    if (pos) { st->pos = usec_to_sec(pos); st->len = usec_to_sec(plen); }
    fix_times(st);
    return 1;
}

/* "0:14" or "1:02:03" -> seconds, *end just past it; -1 if it is not a clock */
static int parse_clock(const char *s, const char **end)
{
    long part[3];
    int n = 0;
    for (;;) {
        long v = 0;
        if (!isdigit((unsigned char)*s)) return -1;
        while (isdigit((unsigned char)*s)) { if (v < 100000) v = v * 10 + (*s - '0'); s++; }
        part[n++] = v;
        if (*s == ':' && n < 3) { s++; continue; }
        break;
    }
    if (n < 2) return -1;
    long sec = n == 2 ? part[0] * 60 + part[1] : part[0] * 3600 + part[1] * 60 + part[2];
    if (end) *end = s;
    return sec > MAX_SEC ? MAX_SEC : (int)sec;
}

/* in "[playing] #3/12   0:14/3:44 (6%)" find the "pos/len" pair of clocks */
static void mpc_times(const char *line, MediaState *st)
{
    for (const char *p = line; *p; p++) {
        const char *e;
        if (!isdigit((unsigned char)*p) || (p > line && !isspace((unsigned char)p[-1]))) continue;
        int a = parse_clock(p, &e);
        if (a < 0 || *e != '/') continue;
        int b = parse_clock(e + 1, NULL);
        if (b < 0) continue;
        st->pos = a; st->len = b;
        return;
    }
}

/* "artist<US>title" (what we ask mpc for), else "Artist - Title", else just a title */
static void split_track(MediaState *st, const char *line)
{
    const char *sep = strchr(line, '\x1f');
    size_t sl = 1;
    if (!sep) { sep = strstr(line, " - "); sl = 3; }
    if (sep) {
        copy_n(st->artist, sizeof st->artist, line, (size_t)(sep - line));
        copy_clip(st->title, sizeof st->title, sep + sl);
    } else copy_clip(st->title, sizeof st->title, line);
}

/* mpc -f '[%artist%]<US>[%title%|%file%]' status:
 *   Artist<US>Title
 *   [playing] #3/12   0:14/3:44 (6%)
 *   volume: 80%   repeat: off   random: off   single: off   consume: off
 * (stopped: only the volume line) */
static int parse_mpc(const char *out, MediaState *st)
{
    state_clear(st);
    st->ps = PS_STOPPED;
    int line_no = 0, seen = 0;
    const char *p = out;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        copy_n(line, sizeof line, p, len);
        p += len;
        if (*p == '\n') p++;

        if (!strncmp(line, "volume:", 7)) {
            const char *v = line + 7;
            while (*v == ' ') v++;
            if (isdigit((unsigned char)*v)) st->vol = atoi(v);
            seen = 1;
        } else if (!strncmp(line, "[playing]", 9)) { st->ps = PS_PLAYING; mpc_times(line + 9, st); seen = 1; }
        else if (!strncmp(line, "[paused]", 8)) { st->ps = PS_PAUSED; mpc_times(line + 8, st); seen = 1; }
        else if (line_no == 0 && line[0] && strncmp(line, "ERROR", 5)) {
            split_track(st, line);
            seen = 1;
        }
        line_no++;
    }
    fix_times(st);
    return seen;
}

/* ------------------------------------------------------------------ */
/* backends                                                            */
/* ------------------------------------------------------------------ */
enum { ACT_QUERY, ACT_TOGGLE, ACT_NEXT, ACT_PREV, ACT_STOP, ACT_VOLUME, ACT_SEEK };
typedef struct { char *argv[12]; char a[24]; char b[48]; } Cmd;

enum { BK_PLAYERCTL, BK_MPC };
typedef struct { int kind; const char *player; } Backend;

static void backend_init(Backend *b, int kind, const char *player)
{
    b->kind = kind;
    b->player = player;
}

static const char *backend_name(const Backend *b) { return b->kind == BK_MPC ? "mpc" : "playerctl"; }

/* ACT_VOLUME: arg = signed percent.  ACT_SEEK: arg = target in permille of `len` seconds. */
static void fill_playerctl(const char *player, Cmd *c, int act, int arg, int len)
{
    int n = 0;
    c->argv[n++] = (char *)"playerctl";
    if (player) { c->argv[n++] = (char *)"-p"; c->argv[n++] = (char *)player; }
    switch (act) {
    case ACT_QUERY:
        c->argv[n++] = (char *)"metadata";
        c->argv[n++] = (char *)"--format";
        c->argv[n++] = (char *)"{{status}}\t{{volume}}\t{{artist}}\t{{title}}\t{{position}}\t{{mpris:length}}";
        break;
    case ACT_TOGGLE: c->argv[n++] = (char *)"play-pause"; break;
    case ACT_NEXT:   c->argv[n++] = (char *)"next"; break;
    case ACT_PREV:   c->argv[n++] = (char *)"previous"; break;
    case ACT_STOP:   c->argv[n++] = (char *)"stop"; break;
    case ACT_VOLUME:
        snprintf(c->a, sizeof c->a, "%.2f%c", (arg < 0 ? -arg : arg) / 100.0, arg < 0 ? '-' : '+');
        c->argv[n++] = (char *)"volume";
        c->argv[n++] = c->a;
        break;
    case ACT_SEEK:
        snprintf(c->a, sizeof c->a, "%d", (int)((long long)len * clamp_i(arg, 0, 1000) / 1000));
        c->argv[n++] = (char *)"position";
        c->argv[n++] = c->a;
        break;
    }
    c->argv[n] = NULL;
}

static void fill_mpc(Cmd *c, int act, int arg)
{
    int n = 0;
    c->argv[n++] = (char *)"mpc";
    switch (act) {
    case ACT_QUERY:
        c->argv[n++] = (char *)"-f";
        c->argv[n++] = (char *)"[%artist%]\x1f[%title%|%file%]";
        c->argv[n++] = (char *)"status";
        break;
    case ACT_TOGGLE: c->argv[n++] = (char *)"toggle"; break;
    case ACT_NEXT:   c->argv[n++] = (char *)"next"; break;
    case ACT_PREV:   c->argv[n++] = (char *)"prev"; break;
    case ACT_STOP:   c->argv[n++] = (char *)"stop"; break;
    case ACT_VOLUME:
        snprintf(c->a, sizeof c->a, "%+d", arg);
        c->argv[n++] = (char *)"volume";
        c->argv[n++] = c->a;
        break;
    case ACT_SEEK:
        snprintf(c->a, sizeof c->a, "%d%%", (clamp_i(arg, 0, 1000) + 5) / 10);
        c->argv[n++] = (char *)"seek";
        c->argv[n++] = c->a;
        break;
    }
    c->argv[n] = NULL;
}

static void backend_fill(const Backend *b, Cmd *c, int act, int arg, int len)
{
    if (b->kind == BK_MPC) fill_mpc(c, act, arg);
    else fill_playerctl(b->player, c, act, arg, len);
}

static int backend_parse(const Backend *b, const char *txt, MediaState *st)
{
    return b->kind == BK_MPC ? parse_mpc(txt, st) : parse_playerctl(txt, st);
}

/* ------------------------------------------------------------------ */
/* non-blocking subprocess                                              */
/* ------------------------------------------------------------------ */
#define MAXBUF (64 * 1024)

typedef struct {
    pid_t pid;
    int fd, active, overflow;
    char *buf;
    size_t len, cap;
    long long t0;
} Job;

static void job_init(Job *j)
{
    memset(j, 0, sizeof *j);
    j->fd = -1;
}

static int job_start(Job *j, char *const *argv)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    pid_t c = fork();
    if (c < 0) { close(p[0]); close(p[1]); return -1; }
    if (c == 0) {
        /* an ignored SIGPIPE/SIGINT would survive exec: reset them */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); if (dn > 2) close(dn); }
        dup2(p[1], 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    j->pid = c; j->fd = p[0]; j->active = 1; j->len = 0; j->overflow = 0; j->t0 = now_ms();
    if (j->buf) j->buf[0] = 0;
    return 0;
}

/* read what is available; 1 on EOF/error */
static int job_pump(Job *j)
{
    char tmp[4096];
    for (;;) {
        ssize_t n = read(j->fd, tmp, sizeof tmp);
        if (n > 0) {
            if (j->len + (size_t)n >= MAXBUF) { j->overflow = 1; continue; }
            if (j->len + (size_t)n + 1 > j->cap) {
                size_t ncap = (j->len + (size_t)n + 1) * 2;
                char *nb = realloc(j->buf, ncap);
                if (!nb) { j->overflow = 1; return 1; }
                j->buf = nb; j->cap = ncap;
            }
            memcpy(j->buf + j->len, tmp, (size_t)n);
            j->len += (size_t)n;
            j->buf[j->len] = 0;
            continue;
        }
        if (n == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return 1;
    }
}

/* exit status, or -1 if killed/failed */
static int job_finish(Job *j)
{
    int st = 0;
    if (j->fd >= 0) close(j->fd);
    j->fd = -1; j->active = 0;
    pid_t r;
    do r = waitpid(j->pid, &st, 0); while (r < 0 && errno == EINTR);
    if (r != j->pid || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static void job_kill(Job *j) { if (j->active) (void)kill(j->pid, SIGKILL); }
static const char *job_text(const Job *j) { return j->buf ? j->buf : ""; }

static void job_destroy(Job *j)
{
    if (j->active) { kill(j->pid, SIGKILL); (void)job_finish(j); }
    free(j->buf);
    j->buf = NULL;
}

/* ------------------------------------------------------------------ */
/* 3x5 pixel font                                                       */
/* ------------------------------------------------------------------ */
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
    case ',': return G(0,0,0,2,4); case '\'': return G(2,2,0,0,0);
    case '(': return G(1,2,2,2,1); case ')': return G(4,2,2,2,4);
    case '_': return G(0,0,0,0,7);
    default: return 0;
    }
}

static int text_w(const char *t) { int n = (int)strlen(t); return n ? n * 4 - 1 : 0; }

/* Fold arbitrary UTF-8 into what the pixel font can show: upper-case ASCII,
 * runs of blanks collapsed, every non-ASCII character shown as one '?'. */
static void ascii_fold(const char *in, char *out, size_t n)
{
    size_t o = 0;
    int sp = 1;
    if (n == 0) return;
    while (*in && o + 1 < n) {
        unsigned char ch = (unsigned char)*in++;
        char c;
        if (ch >= 0xC0) {                       /* lead byte: one glyph, skip continuations */
            while (((unsigned char)*in & 0xC0) == 0x80) in++;
            c = '?';
        } else if (ch >= 0x80) continue;        /* stray continuation byte */
        else if (isalnum(ch) || strchr("-.:/?!+%,'()_", ch)) c = (char)toupper(ch);
        else c = ' ';
        if (c == ' ') { if (sp) continue; sp = 1; } else sp = 0;
        out[o++] = c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Window Maker theme                                                   */
/*                                                                      */
/* Window Maker keeps the active theme in ~/GNUstep/Defaults/WindowMaker */
/* (a GNUstep text property list) and rewrites it whenever a theme is   */
/* applied. We read the colours out of it, pick the theme's most vivid  */
/* colour as the accent, and derive the whole LCD palette from that.    */
/* The file is re-checked about once a second, so a theme switch shows  */
/* up on the tile without restarting.                                   */
/* ------------------------------------------------------------------ */
typedef struct { unsigned tile, lt, dk, lcd, fg, fgmid, fgdim, warn, warndim; } Theme;

static Display *name_dpy;           /* for X colour names (set once the display is open) */
static Colormap name_cmap;

/* c scaled by num/den per channel */
static unsigned scale_rgb(unsigned c, unsigned num, unsigned den)
{
    return (((c >> 16) & 255) * num / den) << 16 | (((c >> 8) & 255) * num / den) << 8 | ((c & 255) * num / den);
}

/* classic fixed palette (also the fallback when no theme can be read) */
static void theme_classic(Theme *t, unsigned fg)
{
    t->tile = 0xa6a6b6;             /* Window Maker's default tile; only visible with -w / -D */
    t->dk = 0x202020; t->lt = 0x606060; t->lcd = 0x000000;
    t->fg = fg; t->fgdim = (fg >> 1) & 0x7f7f7f;
    t->fgmid = scale_rgb(fg, 3, 4);
    t->warn = 0xffb52e; t->warndim = 0x5a3f0f;
}

static double dabs(double x) { return x < 0 ? -x : x; }

static void rgb_to_hsl(unsigned rgb, double *h, double *s, double *l)
{
    double r = ((rgb >> 16) & 255) / 255.0, g = ((rgb >> 8) & 255) / 255.0, b = (rgb & 255) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn;
    *l = (mx + mn) / 2;
    *h = 0; *s = 0;
    if (d <= 0) return;
    *s = d / (1 - dabs(2 * *l - 1));
    if (mx == r) { *h = (g - b) / d; if (*h < 0) *h += 6; }
    else if (mx == g) *h = (b - r) / d + 2;
    else *h = (r - g) / d + 4;
    *h /= 6;
}

static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static unsigned hsl_to_rgb(double h, double s, double l)
{
    double r, g, b;
    if (s <= 0) r = g = b = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue2rgb(p, q, h + 1.0 / 3); g = hue2rgb(p, q, h); b = hue2rgb(p, q, h - 1.0 / 3);
    }
    return ((unsigned)(r * 255 + 0.5) << 16) | ((unsigned)(g * 255 + 0.5) << 8) | (unsigned)(b * 255 + 0.5);
}

/* Palette from one accent colour: bright LCD digits, near-black tinted LCD,
 * and a contrasting "paused" colour (amber, or sky blue on amber-ish themes). */
static void theme_derive(Theme *t, unsigned accent, unsigned tile)
{
    double h, s, l;
    rgb_to_hsl(accent, &h, &s, &l);
    double s2 = s < 0.12 ? 0.0 : (s < 0.45 ? 0.45 : (s > 0.9 ? 0.9 : s));
    t->tile = tile;
    /* blues are dark at the same HSL lightness: raise it until the digits are readable on black */
    double fl = 0.55;
    unsigned fgc = hsl_to_rgb(h, s2, fl);
    while (fl < 0.80 && (0.2126 * ((fgc >> 16) & 255) + 0.7152 * ((fgc >> 8) & 255) + 0.0722 * (fgc & 255)) < 0.45 * 255) {
        fl += 0.02;
        fgc = hsl_to_rgb(h, s2, fl);
    }
    t->fg = fgc;
    t->fgmid = scale_rgb(fgc, 3, 4);
    t->fgdim = hsl_to_rgb(h, s2 * 0.8, 0.24);
    t->lcd = hsl_to_rgb(h, s2 * 0.6, 0.05);
    t->dk = 0x202020;                       /* bevels: the same greys as wmvol */
    t->lt = 0x606060;
    double dh = dabs(h - 0.11);
    if (s2 > 0 && dh < 0.10) {
        t->warn = hsl_to_rgb(0.55, 0.75, 0.62); t->warndim = hsl_to_rgb(0.55, 0.6, 0.22);
    } else {
        t->warn = 0xffb52e; t->warndim = 0x5a3f0f;
    }
}

/* ---- minimal GNUstep text-plist reader: just enough to pull one key's words ---- */
#define PL_TOK 48
#define PL_MAXTOK 16

/* returns 0 at end, a punctuation char ({ } ( ) = ; ,) or 'w' for a word/string */
static int pl_next(const char **pp, char *tok, size_t n)
{
    const char *p = *pp;
    tok[0] = 0;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    if (!*p) { *pp = p; return 0; }
    if (strchr("{}()=;,", *p)) { *pp = p + 1; return *p; }
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            if (o + 1 < n) tok[o++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && !isspace((unsigned char)*p) && !strchr("{}()=;,\"", *p)) {
            if (o + 1 < n) tok[o++] = *p;
            p++;
        }
    }
    tok[o] = 0;
    *pp = p;
    return 'w';
}

/* Collects the words of the value of top-level `key` (flattening arrays).
 * Returns the number of words, 0 if the key is absent. */
static int pl_get(const char *txt, const char *key, char toks[][PL_TOK], int max)
{
    const char *p = txt;
    char tok[PL_TOK];
    int t = pl_next(&p, tok, sizeof tok);
    if (t != '{') p = txt;                      /* tolerate a missing outer brace */
    for (;;) {
        t = pl_next(&p, tok, sizeof tok);
        if (t == 0 || t == '}') return 0;
        if (t != 'w') continue;
        char name[PL_TOK];
        strcpy(name, tok);
        if (pl_next(&p, tok, sizeof tok) != '=') continue;
        int match = !strcmp(name, key), n = 0, nest = 0;
        for (;;) {
            t = pl_next(&p, tok, sizeof tok);
            if (t == 0) return match ? n : 0;
            if (t == ';' && nest == 0) break;
            if (t == '(' || t == '{') nest++;
            else if (t == ')' || t == '}') nest--;
            else if (t == 'w' && match && n < max) { strcpy(toks[n], tok); n++; }
        }
        if (match) return n;
    }
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "#rgb" "#rrggbb" "rgb:r/g/b" "white" "black" "grayNN", else X's colour database */
static int parse_color(const char *s, unsigned *out)
{
    size_t n = strlen(s);
    if (s[0] == '#' && (n == 7 || n == 4)) {
        unsigned v[3];
        for (int i = 0; i < 3; i++) {
            int a, b;
            if (n == 7) { a = hexval(s[1 + 2 * i]); b = hexval(s[2 + 2 * i]); }
            else { a = b = hexval(s[1 + i]); }
            if (a < 0 || b < 0) return 0;
            v[i] = (unsigned)(a * 16 + b);
        }
        *out = (v[0] << 16) | (v[1] << 8) | v[2];
        return 1;
    }
    if (!strncmp(s, "rgb:", 4)) {
        unsigned v[3];
        const char *p = s + 4;
        for (int i = 0; i < 3; i++) {
            unsigned x = 0, max = 0;
            int digits = 0;
            while (hexval(*p) >= 0 && digits < 4) { x = x * 16 + (unsigned)hexval(*p); max = max * 16 + 15; digits++; p++; }
            if (!digits || (i < 2 && *p++ != '/')) return 0;
            v[i] = (x * 255 + max / 2) / max;
        }
        *out = (v[0] << 16) | (v[1] << 8) | v[2];
        return 1;
    }
    if (!strcasecmp(s, "white")) { *out = 0xffffff; return 1; }
    if (!strcasecmp(s, "black")) { *out = 0x000000; return 1; }
    if (!strncasecmp(s, "gray", 4) || !strncasecmp(s, "grey", 4)) {
        const char *d = s + 4;
        int ok = 1, val = 0;
        if (!*d) val = 190;
        else {
            for (const char *q = d; *q; q++) if (!isdigit((unsigned char)*q)) ok = 0;
            if (ok && n <= 7) { int pct = atoi(d); if (pct > 100) ok = 0; else val = (pct * 255 + 50) / 100; }
            else ok = 0;
        }
        if (ok) { *out = ((unsigned)val << 16) | ((unsigned)val << 8) | (unsigned)val; return 1; }
    }
    if (name_dpy && n < 64) {
        XColor c;
        if (XParseColor(name_dpy, name_cmap, s, &c)) {
            *out = ((unsigned)(c.red >> 8) << 16) | ((unsigned)(c.green >> 8) << 8) | (unsigned)(c.blue >> 8);
            return 1;
        }
    }
    return 0;
}

static double chroma(unsigned rgb)
{
    double h, s, l;
    rgb_to_hsl(rgb, &h, &s, &l);
    return s * (1 - dabs(2 * l - 1));
}

/* Builds a theme from the text of ~/GNUstep/Defaults/WindowMaker.
 * Returns 1 if an accent colour was found, else leaves *t untouched. */
static int theme_from_text(const char *txt, Theme *t)
{
    static const char *keys[] = { "FTitleBack", "IconTitleBack", "MenuTitleBack", "MenuTextBack", "UTitleBack" };
    char toks[PL_MAXTOK][PL_TOK];
    unsigned best = 0;
    double bc = -1;
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        int n = pl_get(txt, keys[k], toks, PL_MAXTOK);
        for (int i = 0; i < n; i++) {
            unsigned c;
            if (parse_color(toks[i], &c) && chroma(c) > bc) { bc = chroma(c); best = c; }
        }
    }
    if (bc < 0) return 0;
    unsigned tile = 0xa6a6b6, c;
    int n = pl_get(txt, "IconBack", toks, PL_MAXTOK);
    for (int i = 0; i < n; i++)
        if (parse_color(toks[i], &c)) { tile = c; break; }
    theme_derive(t, best, tile);
    return 1;
}

/* ------------------------------------------------------------------ */
/* tile                                                                 */
/*                                                                      */
/* The three LCD boxes are wmvol's, to the pixel.  Everything outside   */
/* them is shaped away, so Window Maker's own dock-tile texture frames  */
/* the display and follows whatever theme is active.  The transport     */
/* keys are drawn inside the big box as LCD keys: outlined when idle,   */
/* lit solid while pressed.                                             */
/* ------------------------------------------------------------------ */
enum { C_TILE, C_DK, C_LT, C_LCD, C_GREEN, C_GMID, C_GDIM, C_AMBER, C_ADIM, C_N };
enum { B_ICON, B_TIME, B_MAIN, B_PREV, B_PLAY, B_NEXT, NBOX };
#define LCD_BOXES 3
static const int boxes[NBOX][4] = {     /* x, y, w, h */
    { 4, 4, 22, 13 }, { 28, 4, 32, 13 }, { 4, 20, 56, 40 },     /* wmvol's LCD boxes */
    { 7, 35, 14, 13 }, { 23, 35, 17, 13 }, { 42, 35, 14, 13 } };    /* keys */

#define TICK_X 8                /* text column, as wmvol's flash line */
#define TICK_W 49               /* 12 characters */
#define TITLE_Y 23
#define ARTIST_Y 29
#define BAR_X 7                 /* ten 4px segments on a 5px pitch, like wmvol's volume bar */
#define BAR_Y 50
#define BAR_W 49
#define BAR_H 6
#define TICK_STEP_MS 60
#define TICK_HOLD_MS 1000
#define MASK_BYTES (TILE / 8 * TILE)

typedef struct {
    PlayState ps;
    int pressed;                /* B_PREV..B_NEXT or -1 */
    const char *title, *artist; /* already folded */
    int title_off, artist_off;
    const char *label;          /* time / state text in the small right box */
    int label_c;
    int bar_px, bar_c, bar_dim; /* filled pixels of BAR_W, and the two colours */
} View;

typedef struct {
    Display *dpy; Colormap cmap; GC gc; Pixmap pm;
    unsigned long col[C_N];
    int have_cols;
    unsigned char mask[MASK_BYTES];     /* 1 bit per pixel, LSB = leftmost (XBM order) */
} Tile;

static void tile_set_theme(Tile *t, const Theme *th)
{
    if (t->have_cols) XFreeColors(t->dpy, t->cmap, t->col, C_N, 0);
    unsigned rgb[C_N] = { th->tile, th->dk, th->lt, th->lcd, th->fg, th->fgmid, th->fgdim, th->warn, th->warndim };
    for (int i = 0; i < C_N; i++) {
        XColor c;
        c.red = (unsigned short)(((rgb[i] >> 16) & 255) * 257);
        c.green = (unsigned short)(((rgb[i] >> 8) & 255) * 257);
        c.blue = (unsigned short)((rgb[i] & 255) * 257);
        c.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(t->dpy, t->cmap, &c);
        t->col[i] = c.pixel;
    }
    t->have_cols = 1;
}

static Tile *tile_new(Display *d, Window root, int depth, Colormap cm, const Theme *th)
{
    Tile *t = calloc(1, sizeof *t);
    if (!t) { fputs("wmmedia: out of memory\n", stderr); exit(1); }
    t->dpy = d; t->cmap = cm;
    t->pm = XCreatePixmap(d, root, TILE, TILE, (unsigned)depth);
    t->gc = XCreateGC(d, t->pm, 0, NULL);
    XSetGraphicsExposures(d, t->gc, False);
    tile_set_theme(t, th);
    return t;
}

static void tile_destroy(Tile *t)
{
    if (!t) return;
    if (t->have_cols) XFreeColors(t->dpy, t->cmap, t->col, C_N, 0);
    XFreePixmap(t->dpy, t->pm);
    XFreeGC(t->dpy, t->gc);
    free(t);
}

/* every pixel drawn with anything but C_TILE becomes part of the window shape */
static void tile_fill(Tile *t, int x, int y, int w, int h, int c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > TILE) w = TILE - x;
    if (y + h > TILE) h = TILE - y;
    if (w <= 0 || h <= 0) return;
    XSetForeground(t->dpy, t->gc, t->col[c]);
    XFillRectangle(t->dpy, t->pm, t->gc, x, y, (unsigned)w, (unsigned)h);
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
            unsigned char *byte = &t->mask[yy * (TILE / 8) + xx / 8], bit = (unsigned char)(1 << (xx % 8));
            if (c == C_TILE) *byte &= (unsigned char)~bit; else *byte |= bit;
        }
}

static void tile_glyph(Tile *t, int x, int y, int g, int c, int cx0, int cx1)
{
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 3; k++)
            if ((g >> ((4 - r) * 3 + (2 - k))) & 1) {
                int gx = x + k;
                if (gx >= cx0 && gx + 1 <= cx1) tile_fill(t, gx, y + r, 1, 1, c);
            }
}

static void tile_text(Tile *t, int x, int y, const char *s, int c, int cx0, int cx1)
{
    for (; *s; s++, x += 4) tile_glyph(t, x, y, glyph(*s), c, cx0, cx1);
}

/* wmvol's lcd_box: light edge, dark bevel, black glass */
static void tile_lcd_box(Tile *t, const int *b)
{
    tile_fill(t, b[0], b[1], b[2], b[3], C_LT);
    tile_fill(t, b[0], b[1], b[2] - 1, b[3] - 1, C_DK);
    tile_fill(t, b[0] + 1, b[1] + 1, b[2] - 2, b[3] - 2, C_LCD);
}

/* 7-row triangle: the row length grows from 1 to `len` at the middle row;
 * `left` points it left (flush right), else right (flush left) */
static void tile_wedge(Tile *t, int x, int y, int len, int left, int c)
{
    for (int r = 0; r < 7; r++) {
        int k = 3 - (r > 3 ? r - 3 : 3 - r);            /* 0 at the rim, 3 in the middle */
        int l = 1 + k * (len - 1) / 3;
        tile_fill(t, left ? x + len - l : x, y + r, l, 1, c);
    }
}

enum { I_PREV, I_NEXT, I_PLAY, I_PAUSE, I_STOP };

static void icon_size(int kind, int *w, int *h)
{
    switch (kind) {
    case I_PLAY: case I_PAUSE: *w = 6; *h = 7; break;
    case I_STOP: *w = 6; *h = 6; break;
    default:     *w = 10; *h = 7; break;                /* prev / next */
    }
}

static void tile_icon(Tile *t, int kind, int x, int y, int c)
{
    switch (kind) {
    case I_PREV:
        tile_fill(t, x, y, 1, 7, c);
        tile_wedge(t, x + 2, y, 4, 1, c);
        tile_wedge(t, x + 6, y, 4, 1, c);
        break;
    case I_NEXT:
        tile_wedge(t, x, y, 4, 0, c);
        tile_wedge(t, x + 4, y, 4, 0, c);
        tile_fill(t, x + 9, y, 1, 7, c);
        break;
    case I_PLAY:  tile_wedge(t, x, y, 6, 0, c); break;
    case I_PAUSE:
        tile_fill(t, x, y, 2, 7, c);
        tile_fill(t, x + 4, y, 2, 7, c);
        break;
    case I_STOP:  tile_fill(t, x, y, 6, 6, c); break;
    }
}

/* an LCD key: 1px outline with the corners knocked off, lit solid while pressed */
static void tile_key_box(Tile *t, const int *b, int kind, int down, int on)
{
    int x = b[0], y = b[1], w = b[2], h = b[3], iw, ih;
    int frame = on ? C_GDIM : C_ADIM, ink = on ? C_GREEN : C_ADIM;
    if (down) {
        tile_fill(t, x + 1, y, w - 2, h, ink);
        tile_fill(t, x, y + 1, w, h - 2, ink);
        ink = C_LCD;
    } else {
        tile_fill(t, x + 1, y, w - 2, 1, frame);
        tile_fill(t, x + 1, y + h - 1, w - 2, 1, frame);
        tile_fill(t, x, y + 1, 1, h - 2, frame);
        tile_fill(t, x + w - 1, y + 1, 1, h - 2, frame);
    }
    icon_size(kind, &iw, &ih);
    tile_icon(t, kind, x + (w - iw) / 2, y + (h - ih) / 2, ink);
}

static void tile_render(Tile *t, const View *v)
{
    int on = v->ps != PS_NONE;
    int hot = v->ps == PS_PAUSED || !on ? C_AMBER : C_GREEN;
    memset(t->mask, 0, sizeof t->mask);
    tile_fill(t, 0, 0, TILE, TILE, C_TILE);
    for (int i = 0; i < LCD_BOXES; i++) tile_lcd_box(t, boxes[i]);

    /* box 1: state icon */
    {
        const int *b = boxes[B_ICON];
        int kind = v->ps == PS_PLAYING ? I_PLAY : v->ps == PS_PAUSED ? I_PAUSE : I_STOP, w, h;
        if (on) {
            icon_size(kind, &w, &h);
            tile_icon(t, kind, b[0] + (b[2] - w) / 2, b[1] + (b[3] - h) / 2, v->ps == PS_STOPPED ? C_GDIM : hot);
        } else {
            tile_fill(t, b[0] + 6, b[1] + 6, 10, 1, C_AMBER);      /* "--" */
        }
    }

    /* box 2: time or status */
    {
        const int *b = boxes[B_TIME];
        tile_text(t, b[0] + (b[2] - text_w(v->label)) / 2, 8, v->label, v->label_c, 0, TILE);
    }

    /* box 3: tickers, keys, progress bar */
    {
        int tc = on ? C_GREEN : C_AMBER;
        tile_text(t, TICK_X - v->title_off, TITLE_Y, v->title, tc, TICK_X, TICK_X + TICK_W);
        tile_text(t, TICK_X - v->artist_off, ARTIST_Y, v->artist, on ? C_GMID : C_ADIM, TICK_X, TICK_X + TICK_W);

        tile_key_box(t, boxes[B_PREV], I_PREV, v->pressed == B_PREV, on);
        tile_key_box(t, boxes[B_PLAY], v->ps == PS_PLAYING ? I_PAUSE : I_PLAY, v->pressed == B_PLAY, on);
        tile_key_box(t, boxes[B_NEXT], I_NEXT, v->pressed == B_NEXT, on);

        for (int i = 0; i < 10; i++) {
            int x = BAR_X + 5 * i, lw = v->bar_px - 5 * i;
            tile_fill(t, x, BAR_Y, 4, BAR_H, v->bar_dim);
            if (lw > 4) lw = 4;
            if (lw > 0) tile_fill(t, x, BAR_Y, lw, BAR_H, v->bar_c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* app                                                                  */
/* ------------------------------------------------------------------ */
#define AQ 8
#define FLASH_MS 1500

/* a key under the pointer; the hit areas reach into the gaps so there is no dead pixel between keys */
static int hit_button(int x, int y)
{
    for (int i = B_PREV; i <= B_NEXT; i++)
        if (x >= boxes[i][0] - 1 && x < boxes[i][0] + boxes[i][2] + 1 &&
            y >= boxes[i][1] - 1 && y < boxes[i][1] + boxes[i][3] + 1) return i;
    return -1;
}

static int hit_time(int x, int y)
{
    const int *b = boxes[B_TIME];
    return x >= b[0] && x < b[0] + b[2] && y >= b[1] && y < b[1] + b[3];
}

/* x -> 0..1000 along the progress bar, clamped (used while dragging past either end) */
static int bar_permille(int x) { return clamp_i((x - BAR_X) * 1000 / (BAR_W - 1), 0, 1000); }

/* the bar plus a pixel of slack around it; -1 when (x, y) is elsewhere */
static int hit_bar(int x, int y)
{
    if (x < BAR_X - 1 || x > BAR_X + BAR_W || y < BAR_Y - 1 || y > BAR_Y + BAR_H + 1) return -1;
    return bar_permille(x);
}

static int ticker_offset(int w, long long elapsed)
{
    int over = w - TICK_W;
    if (over <= 0) return 0;
    long long total = 2LL * TICK_HOLD_MS + (long long)over * TICK_STEP_MS;
    long long t = elapsed % total;
    if (t < TICK_HOLD_MS) return 0;
    long long o = (t - TICK_HOLD_MS) / TICK_STEP_MS;
    return o > over ? over : (int)o;
}

/* "3:07", "-3:07", "125:00": minutes never wrap to hours, so it always fits the LCD */
static void fmt_clock(char *out, size_t n, int sec, int neg)
{
    sec = clamp_i(sec, 0, MAX_SEC);
    snprintf(out, n, "%s%d:%02d", neg ? "-" : "", sec / 60, sec % 60);
}

typedef struct {
    Backend backend;
    Job query, act;
    Tile *tile;
    MediaState st;
    long long st_t;                     /* when st was refreshed; the position is extrapolated from here */
    int missing;

    struct { int act, arg; } aq[AQ];    /* queued one-shot actions */
    int aq_head, aq_n;
    int vol_pending; long long vol_t;   /* coalesced wheel steps */
    unsigned gen, qgen;                 /* bumped on every user change */
    long long want_query_at;

    char title[FIELD], artist[FIELD];   /* folded for the pixel font */
    long long tick_t0; int title_off, artist_off;
    int pressed;                        /* B_PREV..B_NEXT or -1 */
    int scrub;                          /* permille while dragging the bar, else -1 */
    int show_remaining;
    long long flash_until; int flash_delta;
    int shown_pos;

    Display *dpy; int scr, depth;
    Window root, win, iconwin, tilewin;
    Visual *vis; Colormap cmap; GC gc;
    Atom wm_delete;

    Theme theme;
    char conf_path[512];
    long th_mtime; long th_size; unsigned long th_ino; int th_seen;
    long long th_check_at;
    int has_shape; unsigned char shape_prev[MASK_BYTES]; int shape_valid;
} App;

static void app_request_query(App *a, long long delay)
{
    long long w = now_ms() + delay;
    if (a->want_query_at < 0 || w < a->want_query_at) a->want_query_at = w;
}

static int app_busy(const App *a) { return a->act.active || a->aq_n > 0 || a->vol_pending != 0; }

/* ---- clock ---- */
static int app_display_pos(const App *a, long long now)
{
    if (a->st.pos < 0) return -1;
    long long p = a->st.pos;
    if (a->st.ps == PS_PLAYING) p += (now - a->st_t) / 1000;
    if (a->st.len > 0 && p > a->st.len) p = a->st.len;
    return (int)p;
}

/* freeze the extrapolated position into st, so a state change continues from where the clock is */
static void app_rebase(App *a)
{
    long long now = now_ms();
    a->st.pos = app_display_pos(a, now);
    a->st_t = now;
}

/* ---- display ---- */
static void app_update_ticker(App *a)
{
    char t[sizeof a->st.title + 32], s[sizeof a->st.artist], ft[sizeof a->title], fa[sizeof a->artist];
    if (a->st.ps == PS_NONE) {
        if (a->missing) snprintf(t, sizeof t, "%s not found", backend_name(&a->backend));
        else copy_clip(t, sizeof t, "no player");
        s[0] = 0;
    } else {
        copy_clip(t, sizeof t, a->st.title[0] ? a->st.title : "no track");
        copy_clip(s, sizeof s, a->st.artist);
    }
    ascii_fold(t, ft, sizeof ft);
    ascii_fold(s, fa, sizeof fa);
    if (strcmp(ft, a->title) || strcmp(fa, a->artist)) {
        memcpy(a->title, ft, sizeof a->title); memcpy(a->artist, fa, sizeof a->artist);
        a->tick_t0 = now_ms(); a->title_off = a->artist_off = 0;
    }
}

static int app_scrolling(const App *a) { return text_w(a->title) > TICK_W || text_w(a->artist) > TICK_W; }

/* The tile's window shape follows what it draws; only touch the X server when the outline
 * actually changed (with the LCD layout it never does after the first frame). */
static void app_update_shape(App *a)
{
    if (!a->has_shape || cfg.windowed) return;
    const unsigned char *bits = a->tile->mask;
    if (a->shape_valid && !memcmp(bits, a->shape_prev, MASK_BYTES)) return;
    memcpy(a->shape_prev, bits, MASK_BYTES);
    a->shape_valid = 1;
    Pixmap m = XCreateBitmapFromData(a->dpy, a->win, (const char *)bits, TILE, TILE);
    XShapeCombineMask(a->dpy, a->win, ShapeBounding, 0, 0, m, ShapeSet);
    XShapeCombineMask(a->dpy, a->iconwin, ShapeBounding, 0, 0, m, ShapeSet);
    XFreePixmap(a->dpy, m);
}

static void app_present(App *a)
{
    XCopyArea(a->dpy, a->tile->pm, a->tilewin, a->gc, 0, 0, TILE, TILE, 0, 0);
    if (!cfg.windowed) XCopyArea(a->dpy, a->tile->pm, a->win, a->gc, 0, 0, TILE, TILE, 0, 0);
}

static void app_redraw(App *a)
{
    long long now = now_ms();
    char lab[24];
    View v;
    memset(&v, 0, sizeof v);
    int on = a->st.ps != PS_NONE, flash = a->flash_until > now;
    int pos = a->scrub >= 0 && a->st.len > 0 ? (int)((long long)a->st.len * a->scrub / 1000) : app_display_pos(a, now);
    a->shown_pos = pos;

    v.ps = a->st.ps; v.pressed = a->pressed;
    v.title = a->title; v.artist = a->artist; v.title_off = a->title_off; v.artist_off = a->artist_off;

    if (flash) {
        if (a->st.vol >= 0) snprintf(lab, sizeof lab, "VOL %d", a->st.vol);
        else snprintf(lab, sizeof lab, "VOL %c", a->flash_delta < 0 ? '-' : '+');
        v.label_c = C_GREEN;
    } else if (!on) {
        snprintf(lab, sizeof lab, "%s", a->missing ? "NO CTL" : "NO PLR");
        v.label_c = C_AMBER;
    } else if (a->st.ps == PS_STOPPED || pos < 0) {
        snprintf(lab, sizeof lab, "%s", a->st.ps == PS_PLAYING ? "PLAY" : a->st.ps == PS_PAUSED ? "PAUSE" : "STOP");
        v.label_c = a->st.ps == PS_PLAYING ? C_GREEN : a->st.ps == PS_PAUSED ? C_AMBER : C_GDIM;
    } else {
        int rem = a->show_remaining && a->st.len > 0;
        fmt_clock(lab, sizeof lab, rem ? a->st.len - pos : pos, rem);
        v.label_c = a->st.ps == PS_PAUSED ? C_AMBER : C_GREEN;
    }
    v.label = lab;

    v.bar_c = !on || a->st.ps == PS_PAUSED ? C_AMBER : C_GREEN;
    v.bar_dim = !on || a->st.ps == PS_PAUSED ? C_ADIM : C_GDIM;
    if (flash && a->st.vol >= 0) { v.bar_px = clamp_i(a->st.vol, 0, 100) * BAR_W / 100; v.bar_c = C_GREEN; v.bar_dim = C_GDIM; }
    else if (on && a->st.len > 0 && pos >= 0) v.bar_px = (int)((long long)pos * BAR_W / a->st.len);

    tile_render(a->tile, &v);
    app_update_shape(a);
    app_present(a);
}

/* ---- actions ---- */
static void app_queue_action(App *a, int act, int arg)
{
    if (a->aq_n >= AQ) return;
    int i = (a->aq_head + a->aq_n++) % AQ;
    a->aq[i].act = act; a->aq[i].arg = arg;
    a->gen++;
}

static void app_do_action(App *a, int act)
{
    if (a->st.ps == PS_NONE) return;    /* no player: the keys are dead */
    /* optimistic UI: show the result now, the next poll confirms it */
    app_rebase(a);
    if (act == ACT_TOGGLE) a->st.ps = a->st.ps == PS_PLAYING ? PS_PAUSED : PS_PLAYING;
    if (act == ACT_STOP) { a->st.ps = PS_STOPPED; a->st.pos = -1; }
    app_queue_action(a, act, 0);
    app_update_ticker(a);
    app_redraw(a);
}

static void app_seek_to(App *a, int permille)
{
    if (a->st.ps == PS_NONE || a->st.len <= 0) return;
    permille = clamp_i(permille, 0, 1000);
    a->st.pos = (int)((long long)a->st.len * permille / 1000);
    a->st_t = now_ms();
    app_queue_action(a, ACT_SEEK, permille);
}

static void app_wheel(App *a, int dir, int fine)
{
    int d = dir * (fine ? 1 : cfg.step);
    if (a->st.ps == PS_NONE) return;
    a->vol_pending += d;
    if (a->vol_pending > 100) a->vol_pending = 100;
    if (a->vol_pending < -100) a->vol_pending = -100;
    a->vol_t = now_ms();
    if (a->st.vol >= 0) a->st.vol = clamp_i(a->st.vol + d, 0, 100);
    a->flash_until = a->vol_t + FLASH_MS; a->flash_delta = d;
    a->gen++;
    app_redraw(a);
}

/* ---- events ---- */
static void app_button_press(App *a, const XButtonEvent *e)
{
    switch (e->button) {
    case Button1: {
        int b = hit_button(e->x, e->y), pm;
        if (b >= 0) { if (a->st.ps != PS_NONE) { a->pressed = b; app_redraw(a); } }
        else if (hit_time(e->x, e->y)) { a->show_remaining = !a->show_remaining; app_redraw(a); }
        else if ((pm = hit_bar(e->x, e->y)) >= 0 && a->st.ps != PS_NONE && a->st.len > 0) { a->scrub = pm; app_redraw(a); }
        break;
    }
    case Button2: app_do_action(a, ACT_STOP); break;
    case Button4: app_wheel(a, 1, (e->state & ShiftMask) != 0); break;
    case Button5: app_wheel(a, -1, (e->state & ShiftMask) != 0); break;
    }
}

static void app_motion(App *a, const XMotionEvent *e)
{
    if (a->scrub < 0) return;
    int pm = bar_permille(e->x);
    if (pm != a->scrub) { a->scrub = pm; app_redraw(a); }
}

static void app_button_release(App *a, const XButtonEvent *e)
{
    if (e->button != Button1) return;
    if (a->scrub >= 0) {
        int pm = bar_permille(e->x);
        a->scrub = -1;
        app_seek_to(a, pm);
        app_redraw(a);
        return;
    }
    if (a->pressed < 0) return;
    int was = a->pressed;
    a->pressed = -1;
    if (hit_button(e->x, e->y) == was)
        app_do_action(a, was == B_PREV ? ACT_PREV : was == B_NEXT ? ACT_NEXT : ACT_TOGGLE);
    else app_redraw(a);
}

static void app_handle_event(App *a)
{
    XEvent ev;
    XNextEvent(a->dpy, &ev);
    switch (ev.type) {
    case Expose:
        if (!ev.xexpose.count) app_present(a);
        break;
    case ButtonPress:
        if (ev.xbutton.window == a->win || ev.xbutton.window == a->iconwin) app_button_press(a, &ev.xbutton);
        break;
    case ButtonRelease:
        if (ev.xbutton.window == a->win || ev.xbutton.window == a->iconwin) app_button_release(a, &ev.xbutton);
        break;
    case MotionNotify:
        if (ev.xmotion.window == a->win || ev.xmotion.window == a->iconwin) app_motion(a, &ev.xmotion);
        break;
    case ClientMessage:
        if ((Atom)ev.xclient.data.l[0] == a->wm_delete) quit_flag = 1;
        break;
    }
}

/* ---- debug screenshot (-D PREFIX): PREFIX-tile.ppm ---- */
static void app_dump(App *a)
{
    char path[512];
    snprintf(path, sizeof path, "%s-tile.ppm", cfg.dump);
    XImage *im = XGetImage(a->dpy, a->tile->pm, 0, 0, TILE, TILE, AllPlanes, ZPixmap);
    FILE *f = fopen(path, "wb");
    if (!im || !f) { if (im) XDestroyImage(im); if (f) fclose(f); return; }
    unsigned long rm = a->vis->red_mask, gm = a->vis->green_mask, bm = a->vis->blue_mask;
    int rs = 0, gs = 0, bs = 0;
    while (rm && !((rm >> rs) & 1)) rs++;
    while (gm && !((gm >> gs) & 1)) gs++;
    while (bm && !((bm >> bs) & 1)) bs++;
    fprintf(f, "P6\n%d %d\n255\n", TILE, TILE);
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++) {
            unsigned long px = XGetPixel(im, x, y);
            fputc((int)((px & rm) >> rs), f); fputc((int)((px & gm) >> gs), f); fputc((int)((px & bm) >> bs), f);
        }
    fclose(f);
    XDestroyImage(im);
}

/* ---- theme ---- */
static void app_reload_theme(App *a)
{
    Theme t;
    theme_classic(&t, cfg.fg);
    if (cfg.use_theme && a->conf_path[0]) {
        struct stat sb;
        if (stat(a->conf_path, &sb) == 0 && sb.st_size > 0 && sb.st_size < (1 << 20)) {
            FILE *f = fopen(a->conf_path, "r");
            char *buf = f ? malloc((size_t)sb.st_size + 1) : NULL;
            if (buf) {
                size_t n = fread(buf, 1, (size_t)sb.st_size, f);
                buf[n] = 0;
                theme_from_text(buf, &t);       /* no accent found: stays classic */
            }
            free(buf);
            if (f) fclose(f);
        }
    }
    if (memcmp(&t, &a->theme, sizeof t) != 0) {
        a->theme = t;
        tile_set_theme(a->tile, &a->theme);
        app_redraw(a);
    }
}

/* cheap once-a-second check: did Window Maker rewrite its defaults? */
static void app_check_theme(App *a, long long now)
{
    if (!cfg.use_theme || now < a->th_check_at) return;
    a->th_check_at = now + 1000;
    struct stat sb;
    long m = 0, sz = -1; unsigned long ino = 0;
    if (stat(a->conf_path, &sb) == 0) { m = (long)sb.st_mtime; sz = (long)sb.st_size; ino = (unsigned long)sb.st_ino; }
    if (a->th_seen && m == a->th_mtime && sz == a->th_size && ino == a->th_ino) return;
    a->th_seen = 1; a->th_mtime = m; a->th_size = sz; a->th_ino = ino;
    app_reload_theme(a);
}

/* ---- subprocess scheduling ---- */
static void app_start_query(App *a)
{
    Cmd c;
    backend_fill(&a->backend, &c, ACT_QUERY, 0, 0);
    a->qgen = a->gen;
    if (job_start(&a->query, c.argv) < 0) {
        a->missing = 0; state_clear(&a->st); app_update_ticker(a); app_redraw(a);
        a->want_query_at = now_ms() + 5000;
    } else a->want_query_at = -1;
}

static void app_run_timers(App *a, long long now)
{
    if (!a->act.active) {
        Cmd c;
        int start = 0;
        if (a->aq_n > 0) {
            backend_fill(&a->backend, &c, a->aq[a->aq_head].act, a->aq[a->aq_head].arg, a->st.len);
            a->aq_head = (a->aq_head + 1) % AQ; a->aq_n--;
            start = 1;
        } else if (a->vol_pending != 0 && now - a->vol_t >= 25) {
            backend_fill(&a->backend, &c, ACT_VOLUME, a->vol_pending, 0);
            a->vol_pending = 0;
            start = 1;
        }
        if (start && job_start(&a->act, c.argv) < 0) app_request_query(a, 100);
    }
    if (!a->query.active && !app_busy(a) && a->want_query_at >= 0 && now >= a->want_query_at) app_start_query(a);

    if (a->query.active && now - a->query.t0 > 4000) job_kill(&a->query);
    if (a->act.active && now - a->act.t0 > 4000) job_kill(&a->act);

    app_check_theme(a, now);

    int dirty = 0;
    if (a->flash_until && now >= a->flash_until) { a->flash_until = 0; dirty = 1; }
    if (a->st.ps == PS_PLAYING && a->st.pos >= 0 && a->scrub < 0 && app_display_pos(a, now) != a->shown_pos) dirty = 1;
    if (app_scrolling(a)) {
        long long el = now - a->tick_t0;
        int ot = ticker_offset(text_w(a->title), el), oa = ticker_offset(text_w(a->artist), el);
        if (ot != a->title_off || oa != a->artist_off) { a->title_off = ot; a->artist_off = oa; dirty = 1; }
    }
    if (dirty) app_redraw(a);
}

static void app_service_query(App *a, const struct pollfd *p)
{
    if (!a->query.active || p->fd != a->query.fd || !(p->revents & (POLLIN | POLLHUP | POLLERR))) return;
    if (!job_pump(&a->query)) return;
    int code = job_finish(&a->query);
    if (a->qgen != a->gen) { app_request_query(a, 150); return; }   /* user acted meanwhile: stale */

    MediaState ns;
    state_clear(&ns);
    a->missing = code == 127;
    if (code == 0 && !a->query.overflow && backend_parse(&a->backend, job_text(&a->query), &ns)) a->st = ns;
    else state_clear(&a->st);
    a->st_t = now_ms();
    app_request_query(a, a->st.ps == PS_NONE ? 3000 : 2000);
    app_update_ticker(a);
    app_redraw(a);
    if (cfg.dump) { app_dump(a); quit_flag = 1; }
}

static void app_service_action(App *a, const struct pollfd *p)
{
    if (!a->act.active || p->fd != a->act.fd || !(p->revents & (POLLIN | POLLHUP | POLLERR))) return;
    if (!job_pump(&a->act)) return;
    (void)job_finish(&a->act);
    app_request_query(a, 150);
}

/* Window Maker advertises itself on the root window */
static int app_under_window_maker(App *a)
{
    Atom prop = XInternAtom(a->dpy, "_WINDOWMAKER_WM_PROTOCOLS", True), type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (prop == None) return 0;
    if (XGetWindowProperty(a->dpy, a->root, prop, 0, 1, False, AnyPropertyType, &type, &fmt, &n, &after, &data) != Success)
        return 0;
    if (data) XFree(data);
    return type != None;
}

/* ---- window setup ---- */
static void app_setup_window(App *a, int argc, char **argv)
{
    XSizeHints sh;
    XClassHint ch = { (char *)"wmmedia", (char *)"WMMedia" };
    XWMHints wm;
    char *name = (char *)"wmmedia";
    XTextProperty wname;

    memset(&sh, 0, sizeof sh);
    sh.flags = USSize | USPosition | PMinSize | PMaxSize;
    sh.width = sh.min_width = sh.max_width = TILE;
    sh.height = sh.min_height = sh.max_height = TILE;

    unsigned long bg = BlackPixel(a->dpy, a->scr);
    a->win = XCreateSimpleWindow(a->dpy, a->root, 0, 0, TILE, TILE, 0, bg, bg);
    XSetWMNormalHints(a->dpy, a->win, &sh);
    XSetClassHint(a->dpy, a->win, &ch);
    XStringListToTextProperty(&name, 1, &wname);
    XSetWMName(a->dpy, a->win, &wname);
    XFree(wname.value);

    long mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | StructureNotifyMask;
    XSelectInput(a->dpy, a->win, mask);
    a->gc = XCreateGC(a->dpy, a->win, 0, NULL);
    XSetGraphicsExposures(a->dpy, a->gc, False);

    if (cfg.windowed) {
        a->iconwin = 0; a->tilewin = a->win;
        a->wm_delete = XInternAtom(a->dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(a->dpy, a->win, &a->wm_delete, 1);
    } else {
        a->iconwin = XCreateSimpleWindow(a->dpy, a->win, 0, 0, TILE, TILE, 0, bg, bg);
        XSelectInput(a->dpy, a->iconwin, mask);
        a->tilewin = a->iconwin;
        memset(&wm, 0, sizeof wm);
        wm.initial_state = WithdrawnState;
        wm.icon_window = a->iconwin;
        wm.window_group = a->win;
        wm.flags = StateHint | IconWindowHint | WindowGroupHint;
        XSetWMHints(a->dpy, a->win, &wm);
    }
    XSetCommand(a->dpy, a->win, argv, argc);

    /* the shape itself is set per frame in app_update_shape() */
    int ev, er;
    a->has_shape = !cfg.windowed && XShapeQueryExtension(a->dpy, &ev, &er);
    XMapWindow(a->dpy, a->win);
}

static App *app_new(int argc, char **argv)
{
    App *a = calloc(1, sizeof *a);
    if (!a) { fputs("wmmedia: out of memory\n", stderr); exit(1); }
    backend_init(&a->backend, !strcmp(cfg.backend, "mpc") ? BK_MPC : BK_PLAYERCTL, cfg.player);
    job_init(&a->query);
    job_init(&a->act);

    a->dpy = XOpenDisplay(cfg.display);
    if (!a->dpy) { fputs("wmmedia: cannot open display\n", stderr); exit(1); }
    a->scr = DefaultScreen(a->dpy);
    a->root = RootWindow(a->dpy, a->scr);
    a->depth = DefaultDepth(a->dpy, a->scr);
    a->vis = DefaultVisual(a->dpy, a->scr);
    a->cmap = DefaultColormap(a->dpy, a->scr);
    app_setup_window(a, argc, argv);
    if (!cfg.windowed && !app_under_window_maker(a))
        fputs("wmmedia: Window Maker not detected, so this tile will not dock (use -w for a plain window)\n", stderr);
    name_dpy = a->dpy; name_cmap = a->cmap;
    if (cfg.conf) snprintf(a->conf_path, sizeof a->conf_path, "%s", cfg.conf);
    else if (getenv("HOME")) snprintf(a->conf_path, sizeof a->conf_path, "%s/GNUstep/Defaults/WindowMaker", getenv("HOME"));
    theme_classic(&a->theme, cfg.fg);
    a->tile = tile_new(a->dpy, a->root, a->depth, a->cmap, &a->theme);
    app_check_theme(a, 0);

    a->pressed = -1; a->scrub = -1;
    a->show_remaining = cfg.remaining;
    a->want_query_at = 0;
    state_clear(&a->st);
    app_update_ticker(a);
    app_redraw(a);
    return a;
}

static void app_run(App *a)
{
    int xfd = ConnectionNumber(a->dpy);
    while (!quit_flag) {
        while (XPending(a->dpy)) app_handle_event(a);
        if (quit_flag) break;
        long long now = now_ms();
        app_run_timers(a, now);
        XFlush(a->dpy);

        long long to = 1000;
        #define MINTO(t) do { long long _d = (t) - now; if (_d < 0) _d = 0; if (_d < to) to = _d; } while (0)
        if (app_scrolling(a)) { if (to > TICK_STEP_MS) to = TICK_STEP_MS; }
        if (a->flash_until) MINTO(a->flash_until);
        if (a->st.ps == PS_PLAYING && a->st.pos >= 0 && a->scrub < 0) MINTO(now + 1000 - (now - a->st_t) % 1000 + 1);
        if (a->vol_pending) MINTO(a->vol_t + 25);
        if (a->want_query_at >= 0 && !a->query.active) MINTO(a->want_query_at);
        if (a->aq_n > 0 && !a->act.active) to = 0;
        if (a->query.active || a->act.active) { if (to > 250) to = 250; }

        struct pollfd pf[3];
        pf[0].fd = xfd; pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = a->query.active ? a->query.fd : -1; pf[1].events = POLLIN; pf[1].revents = 0;
        pf[2].fd = a->act.active ? a->act.fd : -1; pf[2].events = POLLIN; pf[2].revents = 0;
        poll(pf, 3, (int)to);
        app_service_query(a, &pf[1]);
        app_service_action(a, &pf[2]);
    }
}

static void app_destroy(App *a)
{
    job_destroy(&a->query); job_destroy(&a->act);
    tile_destroy(a->tile);
    if (a->dpy) XCloseDisplay(a->dpy);
    free(a);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
static void usage(const char *p)
{
    printf("wmmedia %s - Window Maker media control dockapp\n\n"
           "usage: %s [options]\n"
           "  -display DPY   X display\n"
           "  -w             run in a normal window instead of docking\n"
           "  -b BACKEND     playerctl (default) or mpc\n"
           "  -p PLAYER      playerctl player name (e.g. spotify, mpv); default: first found\n"
           "  -s N           wheel volume step in percent, 1-50 (default 5)\n"
           "  -r             show remaining time instead of elapsed\n"
           "  -fg RRGGBB     LCD colour (default 20b2aa, the wmvol teal)\n"
           "  -t             take the LCD colour from the Window Maker theme instead\n"
           "  -c FILE        Window Maker defaults file for -t (default ~/GNUstep/Defaults/WindowMaker)\n"
           "  -D PREFIX      debug: write PREFIX-tile.ppm after the first poll and exit\n"
           "  -h, -v         help / version\n\n"
           "tile:  left click on a key     previous / play-pause / next\n"
           "       click or drag the bar   seek\n"
           "       click on the time       elapsed <-> remaining\n"
           "       wheel                   volume        shift+wheel  1%% steps\n"
           "       middle click            stop\n", VERSION, p);
}

static const char *opt_arg(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "wmmedia: option %s needs an argument\n", argv[*i]);
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
        fprintf(stderr, "wmmedia: bad value '%s' for %s\n", v, opt);
        exit(1);
    }
    return n;
}

static int on_x_io_error(Display *d) { (void)d; _exit(1); }

int main(int argc, char **argv)
{
    int fg_given = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-display") || !strcmp(a, "-d")) cfg.display = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-w")) cfg.windowed = 1;
        else if (!strcmp(a, "-b")) {
            cfg.backend = opt_arg(argc, argv, &i);
            if (strcmp(cfg.backend, "playerctl") && strcmp(cfg.backend, "mpc")) {
                fprintf(stderr, "wmmedia: unknown backend '%s'\n", cfg.backend);
                return 1;
            }
        }
        else if (!strcmp(a, "-p")) cfg.player = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-s")) cfg.step = (int)opt_num(a, opt_arg(argc, argv, &i), 10, 1, 50);
        else if (!strcmp(a, "-D")) cfg.dump = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-fg")) {
            const char *h = opt_arg(argc, argv, &i);
            if (*h == '#') h++;
            cfg.fg = (unsigned)opt_num(a, h, 16, 0, 0xffffff);
            fg_given = 1;
        }
        else if (!strcmp(a, "-t")) cfg.use_theme = 1;
        else if (!strcmp(a, "-notheme")) cfg.use_theme = 0;     /* default now; kept for old command lines */
        else if (!strcmp(a, "-r")) cfg.remaining = 1;
        else if (!strcmp(a, "-c")) cfg.conf = opt_arg(argc, argv, &i);
        else if (!strcmp(a, "-v")) { printf("wmmedia %s\n", VERSION); return 0; }
        else { usage(argv[0]); return !strcmp(a, "-h") ? 0 : 1; }
    }

    if (fg_given) cfg.use_theme = 0;        /* a fixed colour always wins over -t */

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_sig;             /* no SA_RESTART: let poll() wake up */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    XSetIOErrorHandler(on_x_io_error);

    App *app = app_new(argc, argv);
    app_run(app);
    app_destroy(app);
    return 0;
}

