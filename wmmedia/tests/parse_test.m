/* Unit tests: parsers, text folding, ticker maths, hit-testing, backend argv.
 * Includes the program itself so sanitizers cover it. */
#define main wmmedia_main
#include "../wmmedia.m"
#undef main

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void test_playerctl(void)
{
    MediaState s;
    CHECK(parse_playerctl("Playing\t0.550000\tDaft Punk\tAround the World\t83000000\t240000000\n", &s));
    CHECK(s.ps == PS_PLAYING && s.vol == 55 && !strcmp(s.artist, "Daft Punk") && !strcmp(s.title, "Around the World"));
    CHECK(s.pos == 83 && s.len == 240);
    CHECK(parse_playerctl("Paused\t1.000000\t\tJust A Title\t0\t61500000\n", &s));
    CHECK(s.ps == PS_PAUSED && s.vol == 100 && s.artist[0] == 0 && !strcmp(s.title, "Just A Title") && s.pos == 0 && s.len == 61);
    CHECK(parse_playerctl("Stopped\t\t\t\t\t\n", &s));
    CHECK(s.ps == PS_STOPPED && s.vol == -1 && s.title[0] == 0 && s.pos == -1 && s.len == -1);
    CHECK(parse_playerctl("playing\t0\tA\tB\t1000000\t2000000", &s) && s.ps == PS_PLAYING && s.vol == 0 && s.pos == 1 && s.len == 2);
    /* a title containing tabs survives the new format ... */
    CHECK(parse_playerctl("Playing\t0.5\tA\tTi\ttle\t5000000\t9000000\n", &s) && !strcmp(s.title, "Ti\ttle") && s.pos == 5 && s.len == 9);
    /* ... and so does the old four-field form */
    CHECK(parse_playerctl("Playing\t0.5\tA\tTitle\n", &s) && !strcmp(s.title, "Title") && s.pos == -1 && s.len == -1);
    CHECK(parse_playerctl("Playing\t0.5\tA\tTitle\twith\ttabs\n", &s) && !strcmp(s.title, "Title\twith\ttabs") && s.pos == -1);
    /* streams: no length; position past the end is clamped; garbage times are unknown, not fatal */
    CHECK(parse_playerctl("Playing\t0.5\tA\tB\t5000000\t0\n", &s) && s.pos == 5 && s.len == -1);
    CHECK(parse_playerctl("Playing\t0.5\tA\tB\t9000000\t4000000\n", &s) && s.pos == 4 && s.len == 4);
    CHECK(parse_playerctl("Playing\t0.5\tA\tB\t\t\n", &s) && s.pos == -1 && s.len == -1);
    CHECK(parse_playerctl("Playing\t0.5\tA\tB\t99999999999999999999\t99999999999999999999\n", &s) && s.pos == MAX_SEC && s.len == MAX_SEC);
    CHECK(!parse_playerctl("", &s));
    CHECK(!parse_playerctl("No players found\n", &s));
    CHECK(!parse_playerctl("Bogus\t1\ta\tb\t1\t2\n", &s));
    CHECK(!parse_playerctl("Playing\t1\ta\n", &s));                                       /* too few fields */
    char big[1200];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    memcpy(big, "Playing\t1\t", 10);
    big[600] = '\t'; big[900] = '\t';
    CHECK(parse_playerctl(big, &s) && strlen(s.artist) == FIELD - 1 && strlen(s.title) == FIELD - 1);   /* clipped, not overrun */
}

static void test_clock(void)
{
    const char *e;
    CHECK(parse_clock("0:14/2:31", &e) == 14 && *e == '/');
    CHECK(parse_clock("1:02:03", NULL) == 3723);
    CHECK(parse_clock("12:00 x", &e) == 720 && *e == ' ');
    CHECK(parse_clock("5", NULL) == -1 && parse_clock("", NULL) == -1 && parse_clock(":30", NULL) == -1);
    CHECK(parse_clock("99999:59", NULL) == MAX_SEC);
    CHECK(usec_to_sec("0") == 0 && usec_to_sec("999999") == 0 && usec_to_sec("1000000") == 1);
    CHECK(usec_to_sec("") == -1 && usec_to_sec("-5") == -1 && usec_to_sec("1.5") == -1 && usec_to_sec("x") == -1);
    char b[24];
    fmt_clock(b, sizeof b, 83, 0);     CHECK(!strcmp(b, "1:23"));
    fmt_clock(b, sizeof b, 83, 1);     CHECK(!strcmp(b, "-1:23"));
    fmt_clock(b, sizeof b, 0, 0);      CHECK(!strcmp(b, "0:00"));
    fmt_clock(b, sizeof b, 3723, 0);   CHECK(!strcmp(b, "62:03"));
    fmt_clock(b, sizeof b, MAX_SEC, 1); CHECK(!strcmp(b, "-5999:59") || strlen(b) <= 8);
    fmt_clock(b, sizeof b, -5, 0);     CHECK(!strcmp(b, "0:00"));            /* never negative */
    /* the longest label must fit the small LCD box (30px of glass) */
    fmt_clock(b, sizeof b, 999 * 60 + 59, 1); CHECK(text_w(b) <= boxes[B_TIME][2] - 2);
}

static void test_mpc(void)
{
    MediaState s;
    CHECK(parse_mpc("Boards of Canada\x1f" "Roygbiv\n[playing] #3/12   0:14/2:31 (9%)\n"
                    "volume: 80%   repeat: off   random: off   single: off   consume: off\n", &s));
    CHECK(s.ps == PS_PLAYING && s.vol == 80 && !strcmp(s.artist, "Boards of Canada") && !strcmp(s.title, "Roygbiv"));
    CHECK(s.pos == 14 && s.len == 151);
    /* the plain "Artist - Title" line older setups print */
    CHECK(parse_mpc("Boards of Canada - Roygbiv\n[playing] #3/12   0:14/2:31 (9%)\n", &s));
    CHECK(!strcmp(s.artist, "Boards of Canada") && !strcmp(s.title, "Roygbiv"));
    CHECK(parse_mpc("just a file name.flac\n[paused]  #1/1   0:00/1:00 (0%)\n", &s));
    CHECK(s.artist[0] == 0 && !strcmp(s.title, "just a file name.flac") && s.ps == PS_PAUSED && s.pos == 0 && s.len == 60);
    CHECK(parse_mpc("\x1f" "Only Title\n[playing] #1/1   1:02:03/1:30:00 (68%)\n", &s));
    CHECK(s.artist[0] == 0 && !strcmp(s.title, "Only Title") && s.pos == 3723 && s.len == 5400);
    CHECK(parse_mpc("X - Y\n[paused]  #1/1   0:00/1:00 (0%)\nvolume:n/a   repeat: off\n", &s));
    CHECK(s.ps == PS_PAUSED && s.vol == -1);
    CHECK(parse_mpc("S\x1f" "T\n[playing] #1/1   0:30/0:00 (0%)\n", &s) && s.pos == 30 && s.len == -1);   /* stream */
    CHECK(parse_mpc("volume: 40%   repeat: off   random: off\n", &s));
    CHECK(s.ps == PS_STOPPED && s.vol == 40 && s.title[0] == 0 && s.pos == -1);
    CHECK(!parse_mpc("", &s));
    CHECK(!parse_mpc("ERROR: something\n", &s));
}

static void test_fold(void)
{
    char o[64];
    ascii_fold("  Hello,   World!  ", o, sizeof o);   CHECK(!strcmp(o, "HELLO, WORLD!"));
    ascii_fold("Caf\xc3\xa9 del Mar", o, sizeof o);   CHECK(!strcmp(o, "CAF? DEL MAR"));
    ascii_fold("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", o, sizeof o); CHECK(!strcmp(o, "???"));
    ascii_fold("\x80\x80ok", o, sizeof o);            CHECK(!strcmp(o, "OK"));
    ascii_fold("a<b>c|d", o, sizeof o);               CHECK(!strcmp(o, "A B C D"));
    ascii_fold("abcdefghij", o, 5);                   CHECK(!strcmp(o, "ABCD"));
    ascii_fold("", o, sizeof o);                      CHECK(o[0] == 0);
    ascii_fold("   ", o, sizeof o);                   CHECK(o[0] == 0);
    CHECK(glyph('A') != 0 && glyph('(') != 0 && glyph('~') == 0);
    CHECK(text_w("") == 0 && text_w("A") == 3 && text_w("AB") == 7);
}

static void test_ticker(void)
{
    const int over = 100 - TICK_W;                                  /* pixels to travel for a 100px line */
    CHECK(ticker_offset(TICK_W, 123456) == 0 && ticker_offset(40, 123456) == 0);   /* fits: never scrolls */
    CHECK(ticker_offset(100, 0) == 0);
    CHECK(ticker_offset(100, TICK_HOLD_MS - 1) == 0);               /* initial hold */
    CHECK(ticker_offset(100, TICK_HOLD_MS + 10 * TICK_STEP_MS) == 10);
    CHECK(ticker_offset(100, TICK_HOLD_MS + over * TICK_STEP_MS) == over);        /* last pixel */
    CHECK(ticker_offset(100, TICK_HOLD_MS + (over + 12) * TICK_STEP_MS) == over); /* end hold */
    long long total = 2LL * TICK_HOLD_MS + (long long)over * TICK_STEP_MS;
    CHECK(ticker_offset(100, total) == 0);                          /* wraps */
    for (long long t = 0; t < 3 * total; t += 7) {
        int o = ticker_offset(100, t);
        CHECK(o >= 0 && o <= over);
    }
}

static void test_hit(void)
{
    /* keys: the hit areas reach one pixel into the gaps, so there is no dead pixel between them */
    CHECK(hit_button(12, 40) == B_PREV && hit_button(30, 40) == B_PLAY && hit_button(50, 40) == B_NEXT);
    CHECK(hit_button(7, 35) == B_PREV && hit_button(20, 47) == B_PREV);                 /* key edges inclusive */
    for (int x = 7; x <= 55; x++) CHECK(hit_button(x, 41) != -1);                          /* contiguous row */
    CHECK(hit_button(5, 40) == -1 && hit_button(57, 40) == -1);                           /* glass margin left/right */
    CHECK(hit_button(30, 32) == -1 && hit_button(30, 50) == -1);                          /* artist line, bar */
    CHECK(hit_button(32, 10) == -1 && hit_button(10, 10) == -1);                          /* top boxes */
    CHECK(hit_button(-1, -1) == -1 && hit_button(63, 63) == -1);
    CHECK(hit_time(40, 10) && hit_time(29, 5) && !hit_time(10, 10) && !hit_time(40, 25) && !hit_time(60, 10));

    /* progress bar: clamped permille, slack around it, nothing outside */
    CHECK(bar_permille(BAR_X) == 0 && bar_permille(BAR_X + BAR_W - 1) == 1000);
    CHECK(bar_permille(-50) == 0 && bar_permille(500) == 1000);
    CHECK(bar_permille(BAR_X + 24) == 500);
    for (int x = BAR_X; x < BAR_X + BAR_W - 1; x++) CHECK(bar_permille(x) <= bar_permille(x + 1));   /* monotonic */
    CHECK(hit_bar(30, BAR_Y + 2) == bar_permille(30) && hit_bar(BAR_X, BAR_Y) == 0);
    CHECK(hit_bar(BAR_X - 1, BAR_Y) == 0 && hit_bar(BAR_X + BAR_W, BAR_Y) == 1000);       /* one pixel of slack */
    CHECK(hit_bar(30, BAR_Y - 3) == -1 && hit_bar(30, BAR_Y + BAR_H + 2) == -1);
    CHECK(hit_bar(2, BAR_Y) == -1 && hit_bar(62, BAR_Y) == -1);
    CHECK(hit_bar(30, 40) == -1);                                                           /* the keys */
    /* the three kinds of target never overlap */
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++)
            CHECK((hit_button(x, y) >= 0) + (hit_time(x, y) != 0) + (hit_bar(x, y) >= 0) <= 1);

    for (int i = 0; i < NBOX; i++)                                      /* every box inside the tile */
        CHECK(boxes[i][0] >= 0 && boxes[i][1] >= 0 && boxes[i][0] + boxes[i][2] <= TILE &&
              boxes[i][1] + boxes[i][3] <= TILE);
    for (int i = B_PREV; i <= B_NEXT; i++)                              /* keys sit inside the big LCD box */
        CHECK(boxes[i][0] > boxes[B_MAIN][0] && boxes[i][1] > boxes[B_MAIN][1] &&
              boxes[i][0] + boxes[i][2] < boxes[B_MAIN][0] + boxes[B_MAIN][2] &&
              boxes[i][1] + boxes[i][3] < boxes[B_MAIN][1] + boxes[B_MAIN][3]);
    CHECK(BAR_X + BAR_W <= boxes[B_MAIN][0] + boxes[B_MAIN][2] - 1 && BAR_Y + BAR_H <= boxes[B_MAIN][1] + boxes[B_MAIN][3] - 1);
    CHECK(TICK_X >= boxes[B_MAIN][0] + 1 && TICK_X + TICK_W <= boxes[B_MAIN][0] + boxes[B_MAIN][2] - 1);
    /* wmvol's three LCD boxes, unchanged */
    CHECK(boxes[0][0] == 4 && boxes[0][1] == 4 && boxes[0][2] == 22 && boxes[0][3] == 13);
    CHECK(boxes[1][0] == 28 && boxes[1][1] == 4 && boxes[1][2] == 32 && boxes[1][3] == 13);
    CHECK(boxes[2][0] == 4 && boxes[2][1] == 20 && boxes[2][2] == 56 && boxes[2][3] == 40);
}

static void join(char *dst, size_t n, const Cmd *c)
{
    dst[0] = 0;
    for (int i = 0; c->argv[i]; i++) { strncat(dst, c->argv[i], n - strlen(dst) - 1); strncat(dst, " ", n - strlen(dst) - 1); }
}

#define FILL(P_, A_, V_, L_) [P_ fill:&c action:A_ arg:V_ len:L_]; join(s, sizeof s, &c)

static void test_backends(void)
{
    char s[512];
    Cmd c;
    WMBackend *p = [[WMPlayerctl alloc] initWithPlayer:NULL];
    FILL(p, ACT_TOGGLE, 0, 0);   CHECK(!strcmp(s, "playerctl play-pause "));
    FILL(p, ACT_NEXT, 0, 0);     CHECK(!strcmp(s, "playerctl next "));
    FILL(p, ACT_PREV, 0, 0);     CHECK(!strcmp(s, "playerctl previous "));
    FILL(p, ACT_STOP, 0, 0);     CHECK(!strcmp(s, "playerctl stop "));
    FILL(p, ACT_VOLUME, 15, 0);  CHECK(!strcmp(s, "playerctl volume 0.15+ "));
    FILL(p, ACT_VOLUME, -5, 0);  CHECK(!strcmp(s, "playerctl volume 0.05- "));
    FILL(p, ACT_VOLUME, -100, 0); CHECK(!strcmp(s, "playerctl volume 1.00- "));
    FILL(p, ACT_SEEK, 500, 240); CHECK(!strcmp(s, "playerctl position 120 "));
    FILL(p, ACT_SEEK, 0, 240);   CHECK(!strcmp(s, "playerctl position 0 "));
    FILL(p, ACT_SEEK, 1000, 241); CHECK(!strcmp(s, "playerctl position 241 "));
    FILL(p, ACT_SEEK, 5000, 100); CHECK(!strcmp(s, "playerctl position 100 "));       /* clamped */
    FILL(p, ACT_SEEK, -5, 100);  CHECK(!strcmp(s, "playerctl position 0 "));
    FILL(p, ACT_SEEK, 1000, MAX_SEC); CHECK(!strcmp(s, "playerctl position 359999 "));
    FILL(p, ACT_QUERY, 0, 0);
    CHECK(!strcmp(c.argv[1], "metadata") && !strcmp(c.argv[2], "--format") && c.argv[4] == NULL);
    CHECK(strstr(c.argv[3], "{{position}}") && strstr(c.argv[3], "{{mpris:length}}"));
    [p destroy];

    p = [[WMPlayerctl alloc] initWithPlayer:"spotify"];
    FILL(p, ACT_NEXT, 0, 0);     CHECK(!strcmp(s, "playerctl -p spotify next "));
    FILL(p, ACT_SEEK, 250, 200); CHECK(!strcmp(s, "playerctl -p spotify position 50 "));
    [p destroy];

    p = [[WMMpc alloc] initWithPlayer:NULL];
    FILL(p, ACT_TOGGLE, 0, 0);   CHECK(!strcmp(s, "mpc toggle "));
    FILL(p, ACT_PREV, 0, 0);     CHECK(!strcmp(s, "mpc prev "));
    FILL(p, ACT_VOLUME, 5, 0);   CHECK(!strcmp(s, "mpc volume +5 "));
    FILL(p, ACT_VOLUME, -10, 0); CHECK(!strcmp(s, "mpc volume -10 "));
    FILL(p, ACT_SEEK, 500, 0);   CHECK(!strcmp(s, "mpc seek 50% "));
    FILL(p, ACT_SEEK, 1000, 0);  CHECK(!strcmp(s, "mpc seek 100% "));
    FILL(p, ACT_SEEK, 0, 0);     CHECK(!strcmp(s, "mpc seek 0% "));
    FILL(p, ACT_QUERY, 0, 0);    CHECK(!strcmp(s, "mpc -f [%artist%]\x1f[%title%|%file%] status "));
    [p destroy];
}

static unsigned lum(unsigned c) { return (unsigned)(0.2126 * ((c >> 16) & 255) + 0.7152 * ((c >> 8) & 255) + 0.0722 * (c & 255)); }

static void test_plist(void)
{
    char t[PL_MAXTOK][PL_TOK];
    const char *doc =
        "/* theme */ {\n"
        "  FTitleColor = white; // comment\n"
        "  FTitleBack = (hgradient, mediumpurple2, \"rgb:43/33/64\");\n"
        "  Other = { FTitleBack = (solid, red); nested = (a, (b, c)); };\n"
        "  Tricky = \"semi;colon = (paren\";\n"
        "  IconTitleBack = \"#7156a9\";\n"
        "  Empty = ();\n"
        "}";
    int n = pl_get(doc, "FTitleBack", t, PL_MAXTOK);
    CHECK(n == 3 && !strcmp(t[0], "hgradient") && !strcmp(t[1], "mediumpurple2") && !strcmp(t[2], "rgb:43/33/64"));
    CHECK(pl_get(doc, "IconTitleBack", t, PL_MAXTOK) == 1 && !strcmp(t[0], "#7156a9"));
    CHECK(pl_get(doc, "FTitleColor", t, PL_MAXTOK) == 1 && !strcmp(t[0], "white"));
    CHECK(pl_get(doc, "Tricky", t, PL_MAXTOK) == 1 && !strcmp(t[0], "semi;colon = (paren"));
    CHECK(pl_get(doc, "Empty", t, PL_MAXTOK) == 0);
    CHECK(pl_get(doc, "nested", t, PL_MAXTOK) == 0);            /* only top-level keys */
    CHECK(pl_get(doc, "Missing", t, PL_MAXTOK) == 0);
    CHECK(pl_get("A = b; C = d;", "C", t, PL_MAXTOK) == 1);     /* no outer braces */
    CHECK(pl_get("", "A", t, PL_MAXTOK) == 0 && pl_get("{", "A", t, PL_MAXTOK) == 0);
    CHECK(pl_get("{ A = (x, y", "A", t, PL_MAXTOK) == 2);       /* truncated file */
    CHECK(pl_get("{ A = (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20); }", "A", t, 4) == 4);  /* max honoured */
    CHECK(pl_get("{ A = \"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghij\"; }", "A", t, PL_MAXTOK) == 1
          && strlen(t[0]) == PL_TOK - 1);                       /* long word clipped, not overrun */
}

static void test_colors(void)
{
    unsigned c;
    CHECK(parse_color("#7156a9", &c) && c == 0x7156a9);
    CHECK(parse_color("#fff", &c) && c == 0xffffff);
    CHECK(parse_color("rgb:43/33/64", &c) && c == 0x433364);
    CHECK(parse_color("rgb:f/0/8", &c) && c == 0xff0088);
    CHECK(parse_color("rgb:ffff/0000/8000", &c) && c == 0xff0080);
    CHECK(parse_color("white", &c) && c == 0xffffff);
    CHECK(parse_color("Black", &c) && c == 0);
    CHECK(parse_color("gray20", &c) && c == 0x333333);
    CHECK(parse_color("grey100", &c) && c == 0xffffff);
    CHECK(parse_color("gray", &c) && c == 0xbebebe);
    CHECK(!parse_color("gray101", &c) && !parse_color("grayx", &c));
    CHECK(!parse_color("#12345", &c) && !parse_color("#gggggg", &c) && !parse_color("rgb:1/2", &c));
    CHECK(!parse_color("rgb:/1/2", &c) && !parse_color("rgb:", &c));
    CHECK(!parse_color("hgradient", &c) && !parse_color("140", &c) && !parse_color("", &c));
}

static void test_theme(void)
{
    Theme t, c;
    theme_classic(&c, 0x20b2aa);
    t = c;
    CHECK(c.fg == 0x20b2aa && c.dk == 0x202020 && c.lt == 0x606060 && c.lcd == 0);          /* wmvol's palette */
    CHECK(lum(c.fg) > lum(c.fgmid) && lum(c.fgmid) > lum(c.fgdim));
    CHECK(!theme_from_text("{ DontLinkWorkspaces = YES; }", &t) && !memcmp(&t, &c, sizeof t));   /* untouched */
    CHECK(!theme_from_text("", &t) && !theme_from_text("garbage ((( ;;; }}}", &t));
    CHECK(theme_from_text("{ IconTitleBack = \"#7156a9\"; IconBack = (solid, \"#445566\"); }", &t));
    CHECK(t.tile == 0x445566);
    CHECK(t.fg != c.fg && lum(t.fg) > lum(t.fgmid) && lum(t.fgmid) > lum(t.fgdim) && lum(t.lcd) < 30);
    /* most vivid colour wins: gray title bar vs. red icon title */
    CHECK(theme_from_text("{ FTitleBack = (hgradient, \"#808080\", black); IconTitleBack = \"#cc2222\"; }", &t));
    double h, s, l;
    rgb_to_hsl(t.fg, &h, &s, &l);
    CHECK(h < 0.03 || h > 0.97);                                 /* red hue */
    CHECK(theme_from_text("{ MenuTitleBack = (solid, black); UTitleBack = \"#202020\"; }", &t));   /* grayscale theme */
    CHECK(((t.fg >> 16) & 255) == ((t.fg >> 8) & 255) && ((t.fg >> 8) & 255) == (t.fg & 255));

    /* every hue, several saturations: digits readable, LCD dark, pause colour differs from play colour */
    for (int deg = 0; deg < 360; deg += 3)
        for (int sat = 0; sat <= 100; sat += 25)
            for (int li = 10; li <= 90; li += 40) {
                unsigned acc = hsl_to_rgb(deg / 360.0, sat / 100.0, li / 100.0);
                theme_derive(&t, acc, 0);
                CHECK(lum(t.fg) >= 100);
                CHECK(lum(t.lcd) <= 40 && lum(t.fg) > lum(t.lcd) + 80);
                CHECK(lum(t.fgmid) >= 60 && lum(t.fgdim) < lum(t.fgmid) && lum(t.fgmid) < lum(t.fg));
                CHECK(t.warn != t.fg);
            }
    CHECK(hsl_to_rgb(0, 0, 1) == 0xffffff && hsl_to_rgb(0, 0, 0) == 0 && hsl_to_rgb(0, 1, 0.5) == 0xff0000);
}

/* every theme Window Maker ships must parse into a sane palette (skipped if not installed) */
static void test_real_styles(void)
{
    static const char *dirs[] = { "/usr/share/WindowMaker/Styles", "/usr/local/share/WindowMaker/Styles" };
    int tested = 0;
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "ls %s/*.style 2>/dev/null", dirs[d]);
        FILE *ls = popen(cmd, "r");
        char path[512];
        while (ls && fgets(path, sizeof path, ls)) {
            path[strcspn(path, "\n")] = 0;
            FILE *f = fopen(path, "r");
            if (!f) continue;
            static char buf[1 << 17];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            Theme t;
            theme_classic(&t, 0x20b2aa);
            CHECK(theme_from_text(buf, &t));
            CHECK(lum(t.fg) >= 100 && lum(t.lcd) <= 40);
            tested++;
        }
        if (ls) pclose(ls);
    }
    printf("(%d installed Window Maker styles checked)\n", tested);
}

static void test_plist_fuzz(void)
{
    static const char seed[] =
        "{ FTitleBack = (hgradient, mediumpurple2, \"rgb:43/33/64\"); IconTitleBack = \"#7156a9\";\n"
        "  /* c */ IconBack = (dgradient, \"rgb:a6/a6/b6\", \"rgb:51/55/61\"); X = { a = (b, c); }; }";
    unsigned rnd = 777;
    char buf[sizeof seed + 64];
    Theme t;
    for (int it = 0; it < 30000; it++) {
        memcpy(buf, seed, sizeof seed);
        size_t n = sizeof seed - 1;
        for (int k = 0; k < 1 + it % 6; k++) {
            rnd = rnd * 1103515245u + 12345u;
            size_t at = (rnd >> 8) % (n + 1);
            rnd = rnd * 1103515245u + 12345u;
            const char junk[] = "{}();=,\"/*\\# x\x01\xff";
            if ((rnd >> 20) % 5 == 0) buf[at] = 0;
            else if (at < n) buf[at] = junk[(rnd >> 12) % (sizeof junk - 1)];
        }
        theme_classic(&t, 0);
        if (theme_from_text(buf, &t)) CHECK(lum(t.fg) >= 60);
    }
}

/* mutation fuzz: nothing may crash or overrun, whatever the player prints */
static void test_fuzz(void)
{
    static const char *seeds[] = {
        "Playing\t0.550000\tDaft Punk\tAround the World\t83000000\t240000000\n",
        "Boards of Canada\x1f" "Roygbiv\n[playing] #3/12   0:14/2:31 (9%)\nvolume: 80%   repeat: off\n" };
    unsigned seed = 12345;
    MediaState s;
    char buf[2048];
    for (int it = 0; it < 30000; it++) {
        const char *src = seeds[it & 1];
        size_t n = strlen(src);
        memcpy(buf, src, n + 1);
        for (int k = 0; k < 1 + it % 5; k++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = (seed >> 8) % (n + 1);
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) % 4 == 0 && at < n) buf[at] = 0;
            else if (at < n) buf[at] = (char)(seed >> 16);
        }
        if (it % 97 == 0) { size_t m = 1500; memset(buf, 'x', m); buf[m] = 0; if (it & 1) buf[7] = '\t'; }
        if (parse_playerctl(buf, &s)) CHECK(s.pos >= -1 && s.len >= -1 && (s.len < 0 || s.pos <= s.len));
        if (parse_mpc(buf, &s)) CHECK(s.pos >= -1 && s.len >= -1 && (s.len < 0 || s.pos <= s.len));
        char o[200];
        ascii_fold(buf, o, sizeof o);
        CHECK(strlen(o) < sizeof o);
    }
}

int main(void)
{
    test_playerctl(); test_clock(); test_mpc(); test_fold(); test_ticker(); test_hit(); test_backends(); test_fuzz();
    test_plist(); test_colors(); test_theme(); test_real_styles(); test_plist_fuzz();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
