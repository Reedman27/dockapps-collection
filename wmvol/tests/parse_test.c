/* Unit test for the `wpctl status` parser. Build: see `make unit`. */
#define main wmvol_main
#include "../wmvol.c"
#undef main

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char *SAMPLE =
"PipeWire 'pipewire-0' [1.2.7, alex@void, cpu:0]\n"
" \xe2\x94\x94\xe2\x94\x80 Clients:\n"
"        33. WirePlumber                         [1.2.7, alex, pid:812]\n"
"\n"
"Audio\n"
" \xe2\x94\x9c\xe2\x94\x80 Devices:\n"
" \xe2\x94\x82      47. Built-in Audio                      [alsa]\n"
" \xe2\x94\x82\n"
" \xe2\x94\x9c\xe2\x94\x80 Sinks:\n"
" \xe2\x94\x82  *   54. Built-in Audio Analog Stereo        [vol: 0.80]\n"
" \xe2\x94\x82      55. HDMI / DisplayPort 1 Output 2.0     [vol: 1.00 MUTED]\n"
" \xe2\x94\x82\n"
" \xe2\x94\x9c\xe2\x94\x80 Sources:\n"
" \xe2\x94\x82  *   56. USB Condenser Microphone            [vol: 1.50]\n"
" \xe2\x94\x82\n"
" \xe2\x94\x94\xe2\x94\x80 Streams:\n"
"\n"
"Video\n"
" \xe2\x94\x9c\xe2\x94\x80 Sinks:\n"
" \xe2\x94\x82      99. Not an audio device                 [vol: 1.00]\n";

int main(void)
{
    CHECK(parse_status(SAMPLE) == 1);
    CHECK(lists[0].n == 2 && lists[1].n == 1);             /* Video section ignored */
    CHECK(lists[0].d[0].id == 54 && lists[0].d[0].def == 1);
    CHECK(!strcmp(lists[0].d[0].name, "Built-in Audio Analog Stereo"));
    CHECK(lists[0].d[0].vol > 0.79 && lists[0].d[0].vol < 0.81 && !lists[0].d[0].muted);
    CHECK(lists[0].d[1].id == 55 && lists[0].d[1].muted && !lists[0].d[1].def);
    CHECK(!strcmp(lists[0].d[1].name, "HDMI / DisplayPort 1 Output 2.0"));   /* digits in name */
    CHECK(lists[1].d[0].id == 56 && lists[1].d[0].vol > 1.49);              /* >100% */

    /* no Audio section: report failure, keep previous lists */
    CHECK(parse_status("Video\n \xe2\x94\x9c\xe2\x94\x80 Sinks:\n") == 0);
    CHECK(lists[0].n == 2);
    CHECK(parse_status("") == 0);

    /* hostile / odd input must not crash (run under ASan/UBSan via `make asan`) */
    static const char *odd[] = {
        "Audio\n \xe2\x94\x9c\xe2\x94\x80 Sinks:\n *  99999999999999999999. X [vol: abc]\n",
        "Audio\n \xe2\x94\x9c\xe2\x94\x80 Sinks:\n * 1. \n * 2.[vol:\n 3. [vol: 0.5 MUTED",
        "Audio\r\n Sinks:\r\n * 1. CRLF [vol: 0.50]\r\n",
    };
    for (size_t i = 0; i < sizeof odd / sizeof *odd; i++) (void)parse_status(odd[i]);

    char *big = malloc(300000);                             /* giant single line */
    memset(big, '9', 299999); big[299999] = 0;
    (void)parse_status(big); free(big);

    char *many = malloc(100000); strcpy(many, "Audio\n \xe2\x94\x9c\xe2\x94\x80 Sinks:\n");
    for (int i = 0; i < 100; i++) sprintf(many + strlen(many), " \xe2\x94\x82      %d. Dev %d [vol: 0.1]\n", i + 1, i);
    CHECK(parse_status(many) == 1 && lists[0].n == MAXDEV);  /* capped, no overflow */
    free(many);

    srand(1234);                                            /* light mutation fuzz */
    size_t L = strlen(SAMPLE); char *b = malloc(L + 1);
    for (int it = 0; it < 20000; it++) {
        memcpy(b, SAMPLE, L + 1);
        for (int k = 1 + rand() % 8; k--; ) b[rand() % L] = (char)(rand() % 3 ? rand() % 256 : '\n');
        parse_status(b);
    }
    free(b);

    /* Regression: a status call that fails *after* the user touched something
     * (so its result is stale) must still schedule another query, otherwise
     * polling silently stops until the next pactl event. */
    {
        char *argv_f[] = { (char *)"false", NULL };
        j_status.fd = -1;
        CHECK(job_start(&j_status, argv_f, 1) == 0);
        j_status.gen = gen;
        want_query_at = -1;                 /* what start_status() leaves behind */
        touch();                            /* user action while the call was running */
        struct pollfd pf = { .fd = j_status.fd, .events = POLLIN, .revents = 0 };
        poll(&pf, 1, 2000);
        service(&pf, &j_status, 0);
        CHECK(!j_status.active);
        CHECK(want_query_at >= 0);          /* old code: stays -1 forever */
    }

    puts(fails ? "parse_test: FAILED" : "parse_test: ok");
    return fails != 0;
}
