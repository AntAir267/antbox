/*
 * The scene driver's waits (xbox_autoplay.c), against a stand-in title: a
 * thread that counts flips and reports file opens through
 * xbox_FileOpenHook, as the kernel does.
 *
 * Checked here:
 *  - a step finds its file whatever the path's case and separators are
 *    ("tracks\\load.dat" in "D:/Tracks/LOAD.DAT");
 *  - a wait on part of a name ("c3_v1\\crash") is met by whichever file of
 *    that family the title opens (crash3.rws where another run opens
 *    crash1.rws);
 *  - after XAP_SYNC only opens from then on count: the same file opened
 *    earlier (while loading) does not end the wait;
 *  - an open that failed (status != 0) is not an open;
 *  - a wait with a flip bound goes on when the file never comes, and the
 *    run still ends ok;
 *  - a wait without one fails the run when its time is up, naming the step.
 */
#include "kernel.h"
#include "xbox_autoplay.h"
#include "xbox_crash.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#define nap_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define nap_ms(ms) usleep((ms) * 1000u)
#endif

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int s_fail;
#define CHECK(c, what) \
    do { if (!(c)) { printf("FAIL: %s\n", what); s_fail = 1; } else printf("ok: %s\n", what); } while (0)

enum { FLIP_MS = 2, F_EARLY = 5, F_LOAD = 15, F_BAD = 40, F_IMPACT = 90 };

static volatile int s_title_stop;
static void *title(void *arg)
{
    (void)arg;
    while (!s_title_stop) {
        uint32_t f = ++g_xbox_flip_count;
        if (xbox_FileOpenHook) {
            if (f == F_EARLY)       /* the loading screen opens it too */
                xbox_FileOpenHook("D:\\tracks\\US\\C3_v1\\crash3.rws", 0);
            if (f == F_LOAD)
                xbox_FileOpenHook("D:/Tracks/LOAD.DAT", 0);
            if (f == F_BAD)         /* not found: no open */
                xbox_FileOpenHook("D:\\tracks\\US\\C3_v1\\crash2.rws", 0xC0000034u);
            if (f == F_IMPACT)
                xbox_FileOpenHook("D:\\tracks\\US\\C3_v1\\crash3.rws", 0);
        }
        nap_ms(FLIP_MS);
    }
    return NULL;
}

static uint32_t s_mark_flip[4];
static volatile int s_done, s_done_ok;
static char s_why[200];

static void on_mark(void *ctx, const char *text, int thousandths)
{
    (void)ctx; (void)text;
    if (thousandths >= 0 && thousandths < 4)
        s_mark_flip[thousandths] = g_xbox_flip_count;
}

static void on_done(void *ctx, int ok, const char *why)
{
    (void)ctx;
    s_done_ok = ok;
    snprintf(s_why, sizeof s_why, "%s", why ? why : "");
    s_done = 1;
}

static int play(const xap_step *steps, int timeout_s)
{
    static xap_run run;
    int i;
    memset(&run, 0, sizeof run);
    run.steps = steps;
    run.pad_file = "autoplay_test_pad.txt";
    run.wait_timeout_s = timeout_s;
    run.mark = on_mark;
    run.done = on_done;
    s_done = 0;
    if (!xbox_AutoplayStart(&run))
        return -2;
    for (i = 0; i < 2000 && !s_done; i++)
        nap_ms(10);
    nap_ms(50);                         /* the driver thread leaves */
    return s_done ? s_done_ok : -3;
}

int main(void)
{
    static const xap_step scene[] = {
        { XAP_WAIT_FILE, "tracks\\load.dat", NULL, 0 },
        { XAP_MARK, "loaded", NULL, 0 },
        { XAP_SYNC, NULL, NULL, 0 },
        { XAP_WAIT_FILE, "c3_v1\\crash", NULL, 2000 },
        { XAP_MARK, "impact", NULL, 1 },
        { XAP_WAIT_FILE, "never.bin", NULL, 30 },
        { XAP_MARK, "went on", NULL, 2 },
        { XAP_END, NULL, NULL, 0 }
    };
    static const xap_step lost[] = {
        { XAP_WAIT_FILE, "never.bin", NULL, 0 },
        { XAP_END, NULL, NULL, 0 }
    };
    pthread_t th;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_create(&th, NULL, title, NULL);

    rc = play(scene, 20);
    CHECK(rc == 1, "the scene ends ok");
    CHECK(s_mark_flip[0] >= F_LOAD && s_mark_flip[0] < F_LOAD + 30,
          "a path in another case and with '/' is found");
    CHECK(s_mark_flip[1] >= F_IMPACT,
          "after XAP_SYNC neither the earlier open nor a failed one ends the wait");
    CHECK(s_mark_flip[1] < F_IMPACT + 30, "a part of a name finds whichever file of the family is opened");
    CHECK(s_mark_flip[2] >= s_mark_flip[1] + 30 && s_mark_flip[2] < s_mark_flip[1] + 80,
          "a wait with a flip bound goes on when the file never comes");

    rc = play(lost, 1);
    CHECK(rc == 0, "a wait without a bound fails the run when its time is up");
    CHECK(strstr(s_why, "never.bin") != NULL, "and names the step");

    s_title_stop = 1;
    pthread_join(th, NULL);
    remove("autoplay_test_pad.txt");
    printf(s_fail ? "autoplay_test: FAILED\n" : "autoplay_test: all ok\n");
    return s_fail;
}
