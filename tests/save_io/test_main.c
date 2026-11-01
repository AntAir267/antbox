/* save_io: recomp_save_dir.h's export and import, on a made save tree.
 *   save_io_test <work dir> */
#include "recomp_save_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#include <process.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <unistd.h>
#endif

#define ID "4541005b"

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static char g_work[700];

static const char *in(const char *rel)
{
    static char p[8][1024];
    static int k;
    char *o = p[k++ & 7];
    snprintf(o, sizeof p[0], "%s/%s", g_work, rel);
    return o;
}

static void dirs(const char *rel)
{
    char p[1024];
    size_t i;
    snprintf(p, sizeof p, "%s", in(rel));
    for (i = strlen(g_work) + 1; p[i]; i++)
        if (p[i] == '/') { p[i] = 0; mkdir(p, 0755); p[i] = '/'; }
    mkdir(p, 0755);
}

static void put(const char *rel, const char *text)
{
    char d[1024];
    char *slash;
    FILE *f;
    snprintf(d, sizeof d, "%s", rel);
    slash = strrchr(d, '/');
    if (slash) { *slash = 0; dirs(d); }
    f = fopen(in(rel), "wb");
    if (!f) { CHECK("put", 0); return; }
    fwrite(text, 1, strlen(text) + 1, f);        /* with the NUL: not a text file */
    fclose(f);
}

static int has(const char *path, const char *text)
{
    char buf[256] = "";
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return n == strlen(text) + 1 && !strcmp(buf, text);
}

static int there(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static const char *join(const char *a, const char *b)
{
    static char p[8][1400];
    static int k;
    char *o = p[k++ & 7];
    snprintf(o, sizeof p[0], "%s/%s", a, b);
    return o;
}

/* A save root as a start leaves it: this title's save and title data, and
 * what must not travel with it. */
static void make_root(const char *root, const char *mark)
{
    char rel[256];
    snprintf(rel, sizeof rel, "%s/UDATA/" ID "/TitleMeta.xbx", root);            put(rel, "meta");
    snprintf(rel, sizeof rel, "%s/UDATA/" ID "/57BD267AFF58/Profile 1", root);    put(rel, mark);
    snprintf(rel, sizeof rel, "%s/UDATA/" ID "/57BD267AFF58/SaveMeta.xbx", root); put(rel, "savemeta");
    snprintf(rel, sizeof rel, "%s/TDATA/" ID "/cache.dat", root);                 put(rel, mark);
    snprintf(rel, sizeof rel, "%s/TDATA/FFFE0000/MUSIC.HOST/ST.DB", root);        put(rel, "music database");
    snprintf(rel, sizeof rel, "%s/UDATA/11112222/other title", root);             put(rel, "not ours");
    snprintf(rel, sizeof rel, "%s/Partition1.img", root);                         put(rel, "image");
    snprintf(rel, sizeof rel, "%s/settings.toml", root);                          put(rel, "settings");
}

/* Every entry under `dir` with its size, as one text: a listing to compare. */
#if !defined(_WIN32)
#include <dirent.h>
static void listing(const char *dir, const char *rel, char *out, size_t cap)
{
    char path[1400], sub[700];
    struct dirent *e;
    DIR *d;
    snprintf(path, sizeof path, "%s%s%s", dir, *rel ? "/" : "", rel);
    d = opendir(path);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        char full[1700];
        size_t at = strlen(out);
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", dir, sub);
        if (stat(full, &st) != 0)
            continue;
        snprintf(out + at, at < cap ? cap - at : 0, "%s %lld %ld\n", sub,
                 S_ISDIR(st.st_mode) ? -1LL : (long long)st.st_size, (long)st.st_mtime);
        if (S_ISDIR(st.st_mode))
            listing(dir, sub, out, cap);
    }
    closedir(d);
}

/* What every start of every player runs: the import step with nothing
 * asked for. It must change nothing: no file made, moved, rewritten or
 * removed in a save root as a normal run leaves it, and no folder made
 * where there is no save root yet. */
static void test_nothing_staged(void)
{
    static char before[16384], after[16384];
    char text[256] = "x";

    make_root("N", "profile of N");
    put("N/backup/20260101-000000/UDATA/" ID "/TitleMeta.xbx", "meta");
    put("N/crash/report.txt", "r");
    put("N/logs/run.log", "l");
    before[0] = after[0] = 0;
    listing(in("N"), "", before, sizeof before);
    CHECK("a root to compare", strlen(before) > 200);
    CHECK("nothing staged: 0", recomp_save_import_apply(in("N"), ID, text, sizeof text) == 0 && text[0] == 0);
    CHECK("asked twice: still 0", recomp_save_import_apply(in("N"), ID, text, sizeof text) == 0);
    listing(in("N"), "", after, sizeof after);
    CHECK("the save root is exactly as it was (names, sizes, times)", !strcmp(before, after));
    CHECK("no note, no result, no aside folder", !there(in("N/save-import.txt"))
          && !there(in("N/save-import-result.txt")) && !there(in("N/save-before-import")));
    /* No save root yet (a first start): nothing is made. */
    CHECK("a root that is not there: 0", recomp_save_import_apply(in("not-there-yet"), ID, text, sizeof text) == 0);
    CHECK("and it is still not there", !there(in("not-there-yet")));
    /* The read-only questions the overlay asks make nothing either. */
    CHECK("no export folder is made by looking", !recomp_save_export_newest(in("no-exports"), ID, text, sizeof text)
                                                 && !there(in("no-exports")));
    CHECK("nor by the result or staged questions", !recomp_save_import_result(in("N"), text, sizeof text)
                                                   && !recomp_save_import_staged(in("N"), text, sizeof text));
    after[0] = 0;
    listing(in("N"), "", after, sizeof after);
    CHECK("still exactly as it was", !strcmp(before, after));
}
#else
static void test_nothing_staged(void) {}
#endif

#if defined(_WIN32)
/* Windows: the history copy beside the title's own handles. The title's save
 * opens ask for DELETE access (0xC0110000) and pass its share mode to
 * CreateFile (kernel_file.c); a reader that does not share deleting (the
 * CRT's fopen) cannot open such a file. */
static HANDLE hold(const char *rel, DWORD access, DWORD share)
{
    return CreateFileA(in(rel), access, share, NULL, OPEN_EXISTING, 0, NULL);
}

static unsigned __stdcall let_go(void *h)
{
    Sleep(150);
    CloseHandle((HANDLE)h);
    return 0;
}

static void test_history_beside_open_files(void)
{
    char made[1024], err[3000];
    HANDLE h;

    make_root("W", "profile of W");
    /* As the title holds a save it writes: DELETE access, reading shared. */
    h = hold("W/UDATA/" ID "/57BD267AFF58/SaveMeta.xbx", GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ);
    CHECK("win: the title's handle opens", h != INVALID_HANDLE_VALUE);
    CHECK("win: history beside a handle with DELETE access",
          recomp_save_history_take(in("W"), ID, made, sizeof made)
          && has(join(made, "UDATA/" ID "/57BD267AFF58/SaveMeta.xbx"), "savemeta"));
    CloseHandle(h);

    /* A handle that shares nothing, let go of in 150 ms: the bounded retry. */
    h = hold("W/UDATA/" ID "/57BD267AFF58/Profile 1", GENERIC_READ, 0);
    CHECK("win: a handle sharing nothing opens", h != INVALID_HANDLE_VALUE);
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, let_go, h, 0, NULL));
    CHECK("win: history waits briefly for a file held a moment",
          recomp_save_history_take(in("W"), ID, made, sizeof made)
          && has(join(made, "UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of W"));

    /* Held throughout: no copy, the reason named, the save untouched. */
    h = hold("W/UDATA/" ID "/57BD267AFF58/Profile 1", GENERIC_READ, 0);
    err[0] = 0;
    CHECK("win: a file held throughout: no copy",
          !recomp_save_export(in("W"), ID, in("W/save-history"), NULL, made, sizeof made, err, sizeof err));
    printf("  (%s)\n", err);
    CHECK("win: the reason names the file and Windows error 32",
          strstr(err, "Profile 1") && strstr(err, "Windows error 32"));
    CloseHandle(h);
    CHECK("win: the save itself is untouched", has(in("W/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of W"));
}
#endif

int main(int argc, char **argv)
{
    char out[1024], out2[1024], err[1400], text[2048], newest[1024], staged[1024];
    static char A[1024], B[1024], home[1024];

    if (argc < 2) { printf("usage: save_io_test <work dir>\n"); return 2; }
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    mkdir(g_work, 0755);
    make_root("A", "profile of A");
    make_root("B", "profile of B");
    dirs("empty");
    dirs("home");
    snprintf(A, sizeof A, "%s", in("A"));
    snprintf(B, sizeof B, "%s", in("B"));
    snprintf(home, sizeof home, "%s", in("home/Takedawn-saves"));

    test_nothing_staged();

    /* ── export ── */
    CHECK("A has a save", recomp_save_has_data(A, ID));
    CHECK("an empty root has none", !recomp_save_has_data(in("empty"), ID));
    CHECK("nor one of another title", !recomp_save_has_data(A, "99998888"));
    err[0] = 0;
    CHECK("nothing to export says so", !recomp_save_export(in("empty"), ID, home, NULL, out, sizeof out,
                                                           err, sizeof err) && strstr(err, "no save yet"));
    CHECK("export", recomp_save_export(A, ID, home, "from a test", out, sizeof out, err, sizeof err));
    CHECK("in the home folder, dated", !strncmp(out, home, strlen(home)) && strlen(out) == strlen(home) + 16);
    CHECK("the profile, unchanged", has(join(out, "UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of A"));
    CHECK("the title's meta file", has(join(out, "UDATA/" ID "/TitleMeta.xbx"), "meta"));
    CHECK("the title data", has(join(out, "TDATA/" ID "/cache.dat"), "profile of A"));
    CHECK("not the music database", !there(join(out, "TDATA/FFFE0000")));
    CHECK("not another title", !there(join(out, "UDATA/11112222")));
    CHECK("not the images or settings", !there(join(out, "Partition1.img")) && !there(join(out, "settings.toml")));
    CHECK("the note", there(join(out, "save-export.txt")));
    CHECK("it checks", recomp_save_export_check(out, ID, err, sizeof err));
    /* A second one in the same second gets a name of its own. */
    CHECK("export again", recomp_save_export(A, ID, home, NULL, out2, sizeof out2, err, sizeof err)
                          && strcmp(out, out2) != 0);
    /* A target that cannot be made: a file is in the way. */
    put("home/afile", "x");
    err[0] = 0;
    CHECK("an unwritable target says so", !recomp_save_export(A, ID, in("home/afile/sub"), NULL, out2,
                                                              sizeof out2, err, sizeof err) && err[0]);

    /* ── the check ── */
    err[0] = 0;
    CHECK("another title's export is refused", !recomp_save_export_check(out, "99998888", err, sizeof err)
                                               && strstr(err, "another game"));
    dirs("home/Takedawn-saves/zzz-notes");
    err[0] = 0;
    CHECK("a folder without a save is refused", !recomp_save_export_check(in("home/Takedawn-saves/zzz-notes"),
                                                                           ID, err, sizeof err)
                                                && strstr(err, "not a save of this game"));
    CHECK("a missing folder is refused", !recomp_save_export_check(in("home/none"), ID, err, sizeof err));
    /* A hand copy without the note, the folder spelled another way. */
    put("hand/udata/4541005B/TitleMeta.xbx", "meta");
    put("hand/udata/4541005B/AAAA/Profile 1", "by hand");
    CHECK("a hand copy with the same shape is taken", recomp_save_export_check(in("hand"), ID, err, sizeof err));

    /* ── the newest ── */
    CHECK("the newest export, not the notes folder after it",
          recomp_save_export_newest(home, ID, newest, sizeof newest) && strstr(newest, "zzz") == NULL
          && recomp_save_export_check(newest, ID, NULL, 0));
    CHECK("none where there is none", !recomp_save_export_newest(in("empty"), ID, newest, sizeof newest));
    CHECK("none for another title", !recomp_save_export_newest(home, "99998888", newest, sizeof newest));

    /* ── import: staged, then at start ── */
    CHECK("nothing staged", !recomp_save_import_staged(B, staged, sizeof staged));
    CHECK("apply with nothing staged", recomp_save_import_apply(B, ID, text, sizeof text) == 0);
    err[0] = 0;
    CHECK("staging what is not a save is refused", !recomp_save_import_stage(B, in("home/Takedawn-saves/zzz-notes"),
                                                                             ID, err, sizeof err) && err[0]
                                                   && !recomp_save_import_staged(B, staged, sizeof staged));
    CHECK("stage", recomp_save_import_stage(B, out, ID, err, sizeof err));
    CHECK("staged", recomp_save_import_staged(B, staged, sizeof staged) && !strcmp(staged, out));
    CHECK("B untouched until the start", has(in("B/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of B"));
    recomp_save_import_cancel(B);
    CHECK("cancelled", !recomp_save_import_staged(B, staged, sizeof staged)
                       && recomp_save_import_apply(B, ID, text, sizeof text) == 0);

    CHECK("stage again", recomp_save_import_stage(B, out, ID, err, sizeof err));
    put("B/UDATA/" ID "/57BD267AFF58/only in B", "extra");
    CHECK("apply", recomp_save_import_apply(B, ID, text, sizeof text) == 1);
    CHECK("the report names both folders", strstr(text, out) && strstr(text, "save-before-import"));
    CHECK("B has A's profile", has(in("B/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of A"));
    CHECK("and A's title data", has(in("B/TDATA/" ID "/cache.dat"), "profile of A"));
    CHECK("replaced, not merged", !there(in("B/UDATA/" ID "/57BD267AFF58/only in B")));
    CHECK("the rest of B is as it was", has(in("B/UDATA/11112222/other title"), "not ours")
                                        && has(in("B/TDATA/FFFE0000/MUSIC.HOST/ST.DB"), "music database")
                                        && has(in("B/settings.toml"), "settings"));
    {
        /* The old save, aside: <B>/save-before-import/<stamp>/UDATA/<id>/... */
        const char *at = strstr(text, "is in ");
        char before[1024] = "";
        if (at) {
            snprintf(before, sizeof before, "%s", at + 6);
            before[strcspn(before, "\r\n")] = 0;
            if (before[0] && before[strlen(before) - 1] == '.')
                before[strlen(before) - 1] = 0;
        }
        CHECK("B's old profile was set aside", has(join(before, "UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of B")
                                               && has(join(before, "UDATA/" ID "/57BD267AFF58/only in B"), "extra")
                                               && has(join(before, "TDATA/" ID "/cache.dat"), "profile of B"));
        CHECK("beside the save, in its own folder", !strncmp(before, B, strlen(B))
                                                    && strstr(before, "/save-before-import/"));
    }
    CHECK("done once", !recomp_save_import_staged(B, staged, sizeof staged));
    CHECK("the result is kept for this start", recomp_save_import_result(B, text, sizeof text)
                                               && strstr(text, "Imported"));
    CHECK("and gone at the next", recomp_save_import_apply(B, ID, text, sizeof text) == 0
                                  && !recomp_save_import_result(B, text, sizeof text));

    /* Into a root with no save yet. */
    CHECK("stage into an empty root", recomp_save_import_stage(in("empty"), out, ID, err, sizeof err));
    CHECK("apply there", recomp_save_import_apply(in("empty"), ID, text, sizeof text) == 1
                         && strstr(text, "no save here before")
                         && has(in("empty/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of A"));

    /* The export vanished between asking and the start: nothing changes. */
    make_root("C", "profile of C");
    put("gone/UDATA/" ID "/TitleMeta.xbx", "meta");
    CHECK("stage one that will vanish", recomp_save_import_stage(in("C"), in("gone"), ID, err, sizeof err));
    remove(in("gone/UDATA/" ID "/TitleMeta.xbx"));
    CHECK("apply fails", recomp_save_import_apply(in("C"), ID, text, sizeof text) == -1
                         && strstr(text, "not imported") && strstr(text, "unchanged"));
    CHECK("C is as it was", has(in("C/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of C")
                            && !there(in("C/save-before-import")));
    CHECK("and it is not tried again", recomp_save_import_apply(in("C"), ID, text, sizeof text) == 0);

    /* The hand copy imports too, into the title's own spelling. */
    CHECK("stage the hand copy", recomp_save_import_stage(in("C"), in("hand"), ID, err, sizeof err));
    CHECK("apply it", recomp_save_import_apply(in("C"), ID, text, sizeof text) == 1
                      && has(in("C/UDATA/" ID "/AAAA/Profile 1"), "by hand"));

    {
        char h[1024];
        CHECK("a home folder", recomp_home_dir(h, sizeof h) && h[0]);
    }

    /* The history: what is kept, by made dates. */
    {
        static char made[80][32];
        const char *names[80];
        int keep[80], n = 0, i, kept = 0;
        struct tm tm;
        time_t now, t;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = 126; tm.tm_mon = 9; tm.tm_mday = 12; tm.tm_hour = 12; tm.tm_isdst = -1;
        now = mktime(&tm);
        n = 0;
        for (i = 40; i >= 1; i--) {             /* 40 days, oldest first */
            t = now - (time_t)i * 86400;
            strftime(made[n++], 32, "%Y%m%d-%H%M%S", localtime(&t));
        }
        for (i = 25; i >= 1; i--) {             /* 25 today, a minute apart */
            t = now - (time_t)i * 60;
            strftime(made[n++], 32, "%Y%m%d-%H%M%S", localtime(&t));
        }
        snprintf(made[n++], 32, "%s", "my notes");
        for (i = 0; i < n; i++)
            names[i] = made[i];
        /* sorted as the module sorts them */
        for (i = 1; i < n; i++) {
            int j = i;
            while (j > 0 && strcmp(names[j - 1], names[j]) > 0) {
                const char *x = names[j]; names[j] = names[j - 1]; names[j - 1] = x; j--;
            }
        }
        recomp_save_history_keep(names, n, now, keep);
        for (i = 0; i < n; i++)
            kept += keep[i];
        CHECK("history: the 20 newest are kept, a folder not of a date too",
              keep[n - 2] && keep[n - 21] && !keep[n - 22]
              && !strcmp(names[n - 1], "my notes") && keep[n - 1]);
        /* days 1..6 back: one each; weeks 1..3 back: one each */
        CHECK("history: one a day for a week, one a week for a month, the rest goes",
              kept == 20 + 1 + 6 + 3);
    }

    /* The history on disk: a real folder of a dated name is removed, a
     * link and a folder of another name are not. */
    make_root("H", "profile of H");
    {
        char made[1024];
        int i;
        for (i = 0; i < 25; i++)
            CHECK("history: take 25", recomp_save_history_take(in("H"), ID, made, sizeof made));
        CHECK("history: 25 taken in a second or so are all of today", recomp_save_history_count(in("H"), ID) >= 20);
        put("H/save-history/keep-me/note.txt", "mine");
        recomp_save_history_prune(in("H"), time(NULL) + 40 * 86400);
        CHECK("history: a folder of another name is left alone", there(in("H/save-history/keep-me/note.txt")));
        CHECK("history: the save itself is untouched", has(in("H/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile of H"));
    }
    /* A cheat session's copy kept over the save (recomp_save_sandbox_commit):
     * every file of it in place, whole, through a new file beside the old
     * one and a rename; nothing left beside; the rest of the save stays; a
     * new file of the save's (".~new", a save on its way) in the copy is not
     * carried over. */
    make_root("S", "profile before cheats");
    {
        char sb[1024];
        snprintf(sb, sizeof sb, "%s", in("S/cheat-session"));
        CHECK("sandbox: begin", recomp_save_sandbox_begin(in("S"), sb));
        CHECK("sandbox: the copy holds the save", has(join(sb, "UDATA/" ID "/57BD267AFF58/Profile 1"),
                                                      "profile before cheats"));
        put("S/cheat-session/UDATA/" ID "/57BD267AFF58/Profile 1", "profile with cheats");
        put("S/cheat-session/UDATA/" ID "/57BD267AFF58/Profile 2", "a new profile");
        put("S/cheat-session/UDATA/" ID "/57BD267AFF58/Profile 1.~new", "half a save");
        CHECK("sandbox: commit", recomp_save_sandbox_commit(sb, in("S")));
        CHECK("sandbox: the changed file is in the save",
              has(in("S/UDATA/" ID "/57BD267AFF58/Profile 1"), "profile with cheats"));
        CHECK("sandbox: the new file is in the save", has(in("S/UDATA/" ID "/57BD267AFF58/Profile 2"), "a new profile"));
        CHECK("sandbox: the rest of the save stays", has(in("S/UDATA/" ID "/TitleMeta.xbx"), "meta")
              && has(in("S/UDATA/11112222/other title"), "not ours"));
#if !defined(_WIN32)
        {
            static char list[16384];
            list[0] = 0;
            listing(in("S"), "UDATA", list, sizeof list);
            CHECK("sandbox: nothing left beside the save's files", list[0] && !strstr(list, ".~new"));
        }
#else
        CHECK("sandbox: nothing left beside the save's files",
              !there(in("S/UDATA/" ID "/57BD267AFF58/Profile 1.~new")));
#endif
        recomp_save_sandbox_discard(sb);
        CHECK("sandbox: discard removes the copy", !there(sb));
    }
#if defined(_WIN32)
    test_history_beside_open_files();
#endif
    printf(failures ? "%d FAILED\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
