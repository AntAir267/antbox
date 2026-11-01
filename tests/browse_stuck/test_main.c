/* browse_stuck: the folder browser (xst_browse.c) while its counting thread
 * is held by a folder that does not answer, as a read of a network share
 * that hangs in the kernel (BO-289).
 *
 *   browse_stuck_test <work dir>
 *
 * The hold is a test hook at each folder the count walks into (or a `mark`
 * that does not return): it waits until the test lets it go. */
#include "xbox_soundtrack.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <unistd.h>
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
    else printf("ok   %s\n", what); \
} while (0)

static char g_work[1024];

static const char *in_work(const char *rel)
{
    static char p[4][1400];
    static int k;
    char *o = p[k++ & 3];
    snprintf(o, sizeof p[0], "%s/%s", g_work, rel);
    return o;
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f)
        fclose(f);
}

static uint64_t now_us(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64() * 1000u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static void nap_ms(int ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* ── the hold ──────────────────────────────────────────────────────── */

static volatile int g_armed;       /* the next folder with "jam" in it holds */
static volatile int g_held;        /* a thread is being held */
static volatile int g_release;
static volatile int g_dir_sleep;   /* ms at every folder */

static void hold(void)
{
    g_armed = 0;
    g_held = 1;
    while (!g_release)
        nap_ms(1);
    g_held = 0;
}

static void dir_hook(const char *dir)
{
    if (g_armed && strstr(dir, "jam"))
        hold();
    if (g_dir_sleep)
        nap_ms(g_dir_sleep);
}

static int mark_hold(const char *dir)
{
    if (g_armed && strstr(dir, "jam"))
        hold();
    return strstr(dir, "save") != NULL;
}

static int mark_unknown(const char *dir)
{
    (void)dir;
    return -1;
}

static int wait_for(volatile int *v, int want, int ms)
{
    uint64_t end = now_us() + (uint64_t)ms * 1000u;
    while (*v != want && now_us() < end)
        nap_ms(1);
    return *v == want;
}

static int wait_workers(int want, int ms)
{
    uint64_t end = now_us() + (uint64_t)ms * 1000u;
    while (xsb_workers() != want && now_us() < end)
        nap_ms(1);
    return xsb_workers() == want;
}

static int counted(int ms)
{
    uint64_t end = now_us() + (uint64_t)ms * 1000u;
    for (;;) {
        xsb_row r;
        int i, pending = 0;
        for (i = 0; xsb_row_at(i, &r); i++)
            if (r.songs < 0)
                pending = 1;
        if (!pending)
            return 1;
        if (now_us() >= end)
            return 0;
        nap_ms(2);
    }
}

static int row_named(const char *label, xsb_row *out)
{
    int i;
    for (i = 0; xsb_row_at(i, out); i++)
        if (!strcmp(out->label, label))
            return 1;
    return 0;
}

static int find_row_index(const char *label)
{
    xsb_row r;
    int i;
    for (i = 0; xsb_row_at(i, &r); i++)
        if (!strcmp(r.label, label))
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    char home[1400], media[1400];
    const char *roots[2];
    uint64_t t0, dt;
    xsb_row r;

    if (argc < 2) {
        printf("usage: browse_stuck_test <work dir>\n");
        return 2;
    }
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    mkdir(g_work, 0755);
    snprintf(home, sizeof home, "%s", in_work("home"));
    snprintf(media, sizeof media, "%s", in_work("media"));
    mkdir(home, 0755);
    mkdir(in_work("home/Music"), 0755);
    mkdir(in_work("home/Music/jam"), 0755);
    touch(in_work("home/Music/1.mp3"));
    touch(in_work("home/Music/jam/2.mp3"));
    touch(in_work("home/Music/jam/3.mp3"));
    mkdir(media, 0755);
    mkdir(in_work("media/share"), 0755);
    mkdir(in_work("media/share/album"), 0755);
    touch(in_work("media/share/4.mp3"));
    touch(in_work("media/share/album/5.mp3"));
    mkdir(in_work("media/deep"), 0755);
    {
        char p[1400];
        int a, b;
        for (a = 0; a < 20; a++) {
            snprintf(p, sizeof p, "%s/media/deep/%02d", g_work, a);
            mkdir(p, 0755);
            for (b = 0; b < 20; b++) {
                char q[1500];
                snprintf(q, sizeof q, "%s/%02d", p, b);
                mkdir(q, 0755);
            }
        }
    }
    roots[0] = media;
    roots[1] = NULL;
    xsb_set_roots(home, roots);
    xsb_set_test_hooks(dir_hook, NULL);

    /* 1. The count is held inside Music/jam: the browser closes at once. */
    g_armed = 1;
    g_release = 0;
    xsb_open("");
    CHECK("held: the count is held in a folder", wait_for(&g_held, 1, 2000));
    t0 = now_us();
    xsb_close();
    dt = now_us() - t0;
    printf("     close took %llu us while the count is held\n", (unsigned long long)dt);
    CHECK("held: close returns within 100 ms", dt < 100000);
    CHECK("held: closed", !xsb_is_open() && xsb_rows() == 0);
    CHECK("held: the held thread is still there", xsb_workers() == 1);

    /* 2. Open again while the old thread is held: a new thread counts. */
    t0 = now_us();
    xsb_open("");
    dt = now_us() - t0;
    printf("     reopen took %llu us\n", (unsigned long long)dt);
    CHECK("reopen: within 100 ms", dt < 100000 && xsb_is_open());
    CHECK("reopen: every row counted while the old thread is held", counted(3000) && g_held);
    CHECK("reopen: Music has 3 songs", row_named("Music", &r) && r.songs == 3 && !r.more);
    CHECK("reopen: two threads (one held)", xsb_workers() == 2);

    /* 3. Let the old one go: it leaves on its own, its count is dropped. */
    g_release = 1;
    CHECK("release: the old thread leaves without a join", wait_workers(1, 2000));
    nap_ms(50);
    CHECK("release: the rows are the new count's", row_named("Music", &r) && r.songs == 3 && !r.more
                                                  && row_named("Home", &r) && r.songs == 3);
    xsb_close();
    CHECK("close: the thread leaves", wait_workers(0, 2000));

    /* 4. A slow walk (every folder takes 5 ms): a close stops it inside
     * the walk, not after the row. */
    g_release = 0;
    g_dir_sleep = 5;
    xsb_open("");
    nap_ms(30);
    t0 = now_us();
    xsb_close();
    dt = now_us() - t0;
    CHECK("slow walk: close within 100 ms", dt < 100000);
    CHECK("slow walk: the thread leaves inside the walk", wait_workers(0, 200));
    g_dir_sleep = 0;

    /* 5. A folder on a network share (here: a path part the hook names):
     * its own files are counted, its folders are not walked. */
    xsb_set_test_hooks(dir_hook, "share");
    xsb_open("");
    CHECK("share: counted", counted(3000));
    CHECK("share: its own file, more not walked", row_named("Drive: share", &r) && r.songs == 1 && r.more);
    CHECK("share: a local folder walked", row_named("Music", &r) && r.songs == 3 && !r.more);
    CHECK("share: still selectable", row_named("Drive: share", &r)
                                     && xsb_activate(find_row_index("Drive: share"), NULL, 0, NULL) == XSB_MOVED);
    xsb_close();
    xsb_set_test_hooks(dir_hook, NULL);

    /* 6. A `mark` (the save check) held in a folder: the same. */
    {
        xsb_options o;
        memset(&o, 0, sizeof o);
        o.mark = mark_hold;
        o.start_in = in_work("home/Music");
        g_armed = 1;
        g_release = 0;
        xsb_open_with(&o);
        CHECK("mark held", wait_for(&g_held, 1, 2000));
        t0 = now_us();
        xsb_close();
        dt = now_us() - t0;
        CHECK("mark held: close within 100 ms", dt < 100000);
        o.mark = mark_unknown;
        xsb_open_with(&o);
        CHECK("mark -1: counted while the old one is held", counted(3000));
        CHECK("mark -1: not known (songs 0, more)", xsb_row_at(0, &r) && r.songs == 0 && r.more);
        g_release = 1;
        CHECK("mark held: the old thread leaves", wait_workers(1, 2000));
        xsb_close();
        CHECK("mark: closed, no thread", wait_workers(0, 2000));
    }

    /* 7. The work folder is on a local disk. */
    CHECK("local disk is not slow", !xsb_slow_volume(g_work));

    xsb_set_test_hooks(NULL, NULL);
    xsb_set_roots(NULL, NULL);
    printf(failures ? "browse_stuck: %d FAILED\n" : "browse_stuck: ok\n", failures);
    return failures ? 1 : 0;
}
