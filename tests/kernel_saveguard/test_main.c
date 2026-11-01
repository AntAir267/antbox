/*
 * The save guard (kernel_saveguard.h): a save file is written as a new file
 * beside the old one and renamed over it at the last close.
 *
 *   the first save makes the file; nothing ".~new" is left
 *   the same bytes again: the file is left alone, the hook is not called
 *   new bytes: the hook is called once, then the file is the new one
 *   while the title's handle is open the old file is whole and unchanged
 *   a read right after the close sees the new save
 *   a listing does not show the new file
 *   a file deleted on close is gone
 *   a file outside the save trees is written in place
 *   a quit right after the close waits for the save
 *   the process ended at each step of a save (a child of this test, ended
 *   with _exit by the fault hook): the file is the old save or the new one,
 *   whole, and the next start removes what was left beside it
 *   the rename over the old file fails (as on Windows when another program
 *   holds it; the fault hook): the new save is kept beside it, a read sees
 *   it, the next save's close puts it in place; failing to the end the
 *   overlay is told, a newer save overtakes it, the way out tries once more;
 *   a process that ended with one pending: the next start puts it in place,
 *   or, when it cannot, keeps it in the save history and pending; an older
 *   one is moved to the history, never just removed
 */
#include "kernel.h"
#include "kernel_saveguard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#if defined(_WIN32)
#define SEP "\\"
#else
#define SEP "/"
#endif

#define TITLE "4541005b"
#define SAVE  "\\Device\\Harddisk0\\Partition1\\UDATA\\" TITLE "\\0123456789AB\\Profile 1"
#define SAVE2 "\\Device\\Harddisk0\\Partition1\\UDATA\\" TITLE "\\0123456789AB\\Profile 2"
#define SIZE  59904

static int failures, hooks;
static char g_root[400], g_real[600], g_shadow[600], g_pend[600];

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void on_replace(void) { hooks++; }

/* share: the title's share mode. A handle kept open while this test looks
 * at the file from outside shares reading (FILE_SHARE_READ), or Windows
 * would refuse the look; the guard does not depend on it. */
static NTSTATUS open_share(const char *path, ACCESS_MASK access, ULONG disposition, ULONG share,
                           HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    *out = NULL;
    return xbox_NtCreateFile(out, access, &oa, &ios, NULL, 0x80, share, disposition, 0x60);
}

static NTSTATUS open_file(const char *path, ACCESS_MASK access, ULONG disposition, HANDLE *out)
{
    return open_share(path, access, disposition, 0, out);
}

/* As the title saves: open, set the size, one write from the start, close. */
static int save(const char *path, unsigned char fill, int close_it, HANDLE *left)
{
    static unsigned char buf[SIZE];
    XBOX_FILE_END_OF_FILE_INFORMATION eof;
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER at;
    HANDLE h;

    if (open_share(path, 0xC0110000u, XBOX_FILE_OPEN_IF, close_it ? 0 : 1, &h) != 0 || !h)
        return 0;
    eof.EndOfFile.QuadPart = SIZE;
    xbox_NtSetInformationFile(h, &ios, &eof, sizeof eof, XboxFileEndOfFileInformation);
    memset(buf, fill, sizeof buf);
    at.QuadPart = 0;
    if (xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, buf, SIZE, &at) != 0)
        return 0;
    if (close_it)
        xbox_NtClose(h);
    else
        *left = h;
    return 1;
}

/* Whether the name is there; not an open, which Windows refuses while a
 * handle that shares nothing has the file. */
static int exists(const char *p)
{
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

/* The child runs this program again. cmd.exe (system on Windows) drops the
 * first and last quote of a line that starts with one, so the line is quoted
 * once more there; without it the child never ran and the steps "passed"
 * with the old file untouched. The child's exit status is returned. */
static int run_self(const char *self, const char *args, const char *root)
{
    char cmd[1400];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" %s \"%s\"\"", self, args, root);
#else
    snprintf(cmd, sizeof cmd, "\"%s\" %s \"%s\"", self, args, root);
#endif
    return system(cmd);
}

/* The fault hook ends the child with 90 + its step (kernel_saveguard.c). */
static int child_ended_at(int rc, int step)
{
#if defined(_WIN32)
    return rc == 90 + step;
#else
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 90 + step;
#endif
}

/* fill when every byte of the file is it and the size is SIZE; -1 otherwise */
static int content(const char *p)
{
    static unsigned char buf[SIZE + 16];
    size_t n, i;
#if defined(_WIN32)
    /* Shares deleting too, as the guard's own reads: the title's handle has
     * DELETE access, which a plain fopen cannot open beside. */
    DWORD got = 0;
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    if (!ReadFile(f, buf, sizeof buf, &got, NULL)) got = 0;
    CloseHandle(f);
    n = got;
#else
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
#endif
    if (n != SIZE) return -2;
    for (i = 1; i < n; i++)
        if (buf[i] != buf[0]) return -3;
    return buf[0];
}

/* Through the title's read path, right after the close. */
static int read_back(void)
{
    static unsigned char buf[SIZE];
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER at;
    HANDLE h;
    int v = -1;
    if (open_file(SAVE, 0x80100000u, XBOX_FILE_OPEN, &h) != 0)
        return -1;
    at.QuadPart = 0;
    if (xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, SIZE, &at) == 0 && ios.Information == SIZE)
        v = buf[0] == buf[SIZE - 1] ? buf[0] : -3;
    xbox_NtClose(h);
    return v;
}

static int listed_shadow(void)
{
    static unsigned char info[1024];
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE d;
    int seen_new = 0, seen = 0;
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    const char *path = "\\Device\\Harddisk0\\Partition1\\UDATA\\" TITLE "\\0123456789AB";
    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    if (xbox_NtCreateFile(&d, 0x80100001u, &oa, &ios, NULL, 0, 3, XBOX_FILE_OPEN, 0x4021) != 0)
        return -1;
    for (;;) {
        PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
        if (xbox_NtQueryDirectoryFile(d, NULL, NULL, NULL, &ios, info, sizeof info,
                                      XboxFileDirectoryInformation, NULL, FALSE) != 0)
            break;
        seen++;
        if (e->FileNameLength > 5 && !memcmp(e->FileName + e->FileNameLength - 5, ".~new", 5))
            seen_new = 1;
        if (e->FileNameLength > 6 && !memcmp(e->FileName + e->FileNameLength - 6, ".~pend", 6))
            seen_new = 1;
    }
    xbox_NtClose(d);
    return seen_new ? 1 : seen ? 0 : -1;
}

static void setup(const char *root)
{
    char p[700];
    snprintf(g_root, sizeof g_root, "%s", root);
    xbox_path_init(root, root);
#if defined(_WIN32)
    snprintf(p, sizeof p, "%s\\UDATA", root); CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\UDATA\\" TITLE, root); CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\UDATA\\" TITLE "\\0123456789AB", root); CreateDirectoryA(p, NULL);
#else
    snprintf(p, sizeof p, "%s/UDATA", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/UDATA/" TITLE, root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/UDATA/" TITLE "/0123456789AB", root); mkdir(p, 0755);
#endif
    snprintf(g_real, sizeof g_real, "%s" SEP "UDATA" SEP TITLE SEP "0123456789AB" SEP "Profile 1", root);
    snprintf(g_shadow, sizeof g_shadow, "%s.~new", g_real);
    snprintf(g_pend, sizeof g_pend, "%s.~pend", g_real);
    xbox_SaveGuardEnable(TITLE);
    xbox_SaveGuardSetHook(on_replace);
}

/* The child: the root has save 'B'; write 'C' and end at `step`. */
static int child(int step, const char *root)
{
    setup(root);
    xbox_SaveGuardFaultAt(step);
    save(SAVE, 'C', 1, NULL);
    xbox_SaveGuardWait(5000);
    return 0;
}

/* Folders in <root>/save-history holding a save file of `fill`. */
static int history_with(const char *root, int fill)
{
    char p[900];
    int found = 0;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE f;
    snprintf(p, sizeof p, "%s\\save-history\\*", root);
    f = FindFirstFileA(p, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (fd.cFileName[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s\\save-history\\%s\\UDATA\\" TITLE "\\0123456789AB\\Profile 1", root,
                 fd.cFileName);
        found += content(p) == fill;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
#else
    DIR *d;
    struct dirent *e;
    snprintf(p, sizeof p, "%s/save-history", root);
    d = opendir(p);
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/save-history/%s/UDATA/" TITLE "/0123456789AB/Profile 1", root, e->d_name);
        found += content(p) == fill;
    }
    closedir(d);
#endif
    return found;
}

/* A plain file written from outside (an older pending one, by hand). */
static void put_file(const char *p, unsigned char fill)
{
    static unsigned char buf[SIZE];
    FILE *f = fopen(p, "wb");
    memset(buf, fill, sizeof buf);
    if (f) {
        fwrite(buf, 1, sizeof buf, f);
        fclose(f);
    }
}

static void pause_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

int main(int argc, char **argv)
{
    char root[400], cmd[1200], what[120], text[400];
    HANDLE h = NULL;
    int step;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 4 && !strcmp(argv[1], "fault"))
        return child(atoi(argv[2]), argv[3]);
    if (argc == 3 && !strcmp(argv[1], "pendexit")) {
        /* The rename fails once; the way out tries again. */
        setup(argv[2]);
        xbox_SaveGuardFailReplace(1);
        save(SAVE, 'H', 1, NULL);
        xbox_SaveGuardWait(5000);
        exit(0);
    }
    if (argc == 4 && !strcmp(argv[1], "pendkill")) {
        /* The rename keeps failing, then the process is killed. */
        setup(argv[3]);
        xbox_SaveGuardFailReplace(1000);
        save(SAVE, (unsigned char)argv[2][0], 1, NULL);
        xbox_SaveGuardWait(5000);
        fflush(NULL);
        _exit(0);
    }
    if (argc == 3 && !strcmp(argv[1], "quit")) {
        /* A save, then the process ends at once, the ordinary way. */
        setup(argv[2]);
        save(SAVE, 'D', 1, NULL);
        exit(0);
    }
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(root, sizeof root, "%ssaveguard-%lu", tmp, (unsigned long)GetCurrentProcessId());
        if (!CreateDirectoryA(root, NULL)) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/saveguardXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
    }
#endif
    setup(root);

    check(save(SAVE, 'A', 1, NULL) && xbox_SaveGuardWait(5000), "the first save");
    check(content(g_real) == 'A' && !exists(g_shadow), "  the file is it, nothing left beside it");
    check(hooks == 1, "  the hook was called (there was no file before)");

    check(save(SAVE, 'A', 1, NULL) && xbox_SaveGuardWait(5000), "the same bytes again");
    check(content(g_real) == 'A' && !exists(g_shadow) && hooks == 1,
          "  left alone, the hook not called");

    check(save(SAVE, 'B', 0, &h), "a new save, its handle still open");
    check(content(g_real) == 'A', "  the old file is whole and unchanged meanwhile");
    check(content(g_shadow) == 'B', "  the title's bytes are in the new file");
    check(listed_shadow() == 0, "  a listing shows the save, not the new file");
    xbox_NtClose(h);
    check(read_back() == 'B', "  a read right after the close sees the new save");
    check(xbox_SaveGuardWait(5000) && content(g_real) == 'B' && !exists(g_shadow),
          "  the file is the new save, nothing left beside it");
    check(hooks == 2, "  the hook was called once before it");

    /* A file outside the save trees: in place. */
    {
        char z[700];
        HANDLE hz;
        XBOX_IO_STATUS_BLOCK ios;
        LARGE_INTEGER at;
        snprintf(z, sizeof z, "%s" SEP "loose.bin.~new", root);
        if (open_file("\\Device\\Harddisk0\\Partition1\\loose.bin", 0xC0110000u, XBOX_FILE_OVERWRITE_IF, &hz) == 0) {
            at.QuadPart = 0;
            xbox_NtWriteFile(hz, NULL, NULL, NULL, &ios, "x", 1, &at);
            z[strlen(z) - 5] = 0;
            check(exists(z), "a file outside the save trees is written in place");
            strcat(z, ".~new");
            check(!exists(z), "  nothing beside it");
            xbox_NtClose(hz);
        } else {
            check(0, "a file outside the save trees opens");
        }
    }

    /* The process ended at each step. */
    for (step = XBOX_SAVE_FAULT_AFTER_OPEN; step <= XBOX_SAVE_FAULT_AFTER_REPLACE; step++) {
        int want = step == XBOX_SAVE_FAULT_AFTER_REPLACE ? 'C' : 'B';
        int got;
        int rc;
        snprintf(cmd, sizeof cmd, "fault %d", step);
        rc = run_self(argv[0], cmd, root);
        snprintf(what, sizeof what, "ended at step %d: the child ended there (status %d)", step, rc);
        check(child_ended_at(rc, step), what);
        got = content(g_real);
        snprintf(what, sizeof what, "ended at step %d: the file is the %s save, whole", step,
                 want == 'B' ? "old" : "new");
        check(got == want, what);
        xbox_SaveGuardEnable(TITLE);          /* the next start: the sweep */
        snprintf(what, sizeof what, "  then the next start leaves nothing beside it");
        check(!exists(g_shadow), what);
        if (got == 'C') {                       /* back to B for the next step */
            save(SAVE, 'B', 1, NULL);
            xbox_SaveGuardWait(5000);
        }
    }

    /* The process quits right after the title's close: the save is in. */
    check(run_self(argv[0], "quit", root) == 0, "a quit right after a save: the child ran");
    check(content(g_real) == 'D' && !exists(g_shadow), "a quit right after a save waits for it");

    /* The rename over the old file fails (Windows: another program holds
     * it). Real 'D' here. */
    xbox_SaveGuardFailReplace(1);
    check(save(SAVE, 'E', 1, NULL) && xbox_SaveGuardWait(5000), "the rename fails once");
    check(content(g_real) == 'D' && content(g_pend) == 'E' && !exists(g_shadow),
          "  the old file stands, the new one is kept beside it");
    check(xbox_SaveGuardPending() == 1 && xbox_SaveGuardProblem(text, sizeof text) == 0,
          "  pending, the player not told yet");
    check(read_back() == 'E', "  a read sees the newer save");
    check(listed_shadow() == 0, "  a listing does not show it");
    check(save(SAVE2, 'X', 1, NULL) && xbox_SaveGuardWait(5000), "the next save's close");
    {
        int tries;
        for (tries = 0; tries < 50 && xbox_SaveGuardPending(); tries++)
            pause_ms(20);
    }
    check(content(g_real) == 'E' && !exists(g_pend) && !xbox_SaveGuardPending(),
          "  the waiting save is put in place");

    xbox_SaveGuardFailReplace(1000);
    save(SAVE, 'F', 1, NULL);
    xbox_SaveGuardWait(5000);
    for (step = 0; step < 3; step++) {
        int tries;
        save(SAVE2, (unsigned char)('Y' + (step & 1)), 1, NULL);
        xbox_SaveGuardWait(5000);
        for (tries = 0; tries < 10; tries++)
            pause_ms(20);
    }
    check(xbox_SaveGuardProblem(text, sizeof text) > 0 && strstr(text, "A save could not be written") != NULL
          && strstr(text, "previous save is kept") != NULL, "failing to the end: the overlay is told");
    printf("    (%s)\n", text);
    check(content(g_real) == 'E' && content(g_pend) == 'F' && read_back() == 'F',
          "  the previous save stands, the newer one is kept and read");
    xbox_SaveGuardFailReplace(0);
    check(save(SAVE, 'G', 1, NULL) && save(SAVE2, 'Q', 1, NULL) && xbox_SaveGuardWait(5000),
          "newer saves, the rename works again");
    check(content(g_real) == 'G' && !exists(g_pend) && !exists(g_shadow) && !xbox_SaveGuardPending()
          && xbox_SaveGuardProblem(text, sizeof text) == 0, "  it is in place, the waiting one overtaken, the line gone");

    /* The way out tries once more. */
    check(run_self(argv[0], "pendexit", root) == 0, "the way out with a save pending: the child ran");
    check(content(g_real) == 'H' && !exists(g_pend), "  it was put in place on the way out");

    /* Killed with one pending: the next start puts it in place. */
    run_self(argv[0], "pendkill K", root);
    check(content(g_real) == 'H' && content(g_pend) == 'K', "killed with a save pending: both on the disk");
    xbox_SaveGuardEnable(TITLE);
    check(content(g_real) == 'K' && !exists(g_pend), "  the next start puts it in place");

    /* ... and when it cannot: kept in the history, pending, the player told. */
    run_self(argv[0], "pendkill L", root);
    xbox_SaveGuardFailReplace(1);
    xbox_SaveGuardEnable(TITLE);
    check(content(g_real) == 'K' && content(g_pend) == 'L', "  a start that cannot put it in place keeps it");
    check(history_with(root, 'L') == 1, "  and a whole save with it is in the save history");
    check(xbox_SaveGuardProblem(text, sizeof text) > 0 && read_back() == 'L', "  pending, read, the player told");
    save(SAVE2, 'Z', 1, NULL);
    xbox_SaveGuardWait(5000);
    {
        int tries;
        for (tries = 0; tries < 50 && xbox_SaveGuardPending(); tries++)
            pause_ms(20);
    }
    check(content(g_real) == 'L' && !exists(g_pend) && xbox_SaveGuardProblem(text, sizeof text) == 0,
          "  the next save's close puts it in place");

    /* An older one (the file beside it was written after it): moved to the
     * history, not removed. */
    put_file(g_pend, 'M');
    pause_ms(1100);
    save(SAVE, 'N', 1, NULL);
    xbox_SaveGuardWait(5000);
    xbox_SaveGuardEnable(TITLE);
    check(content(g_real) == 'N' && !exists(g_pend) && history_with(root, 'M') == 1,
          "an older waiting file: the save stands, the file is kept in the history");

    /* Deleted on close, through the handle. */
    {
        XBOX_FILE_DISPOSITION_INFORMATION del;
        XBOX_IO_STATUS_BLOCK ios;
        if (open_file(SAVE, 0xC0110000u, XBOX_FILE_OPEN, &h) == 0) {
            del.DeleteFile = TRUE;
            xbox_NtSetInformationFile(h, &ios, &del, sizeof del, XboxFileDispositionInformation);
            xbox_NtClose(h);
            xbox_SaveGuardWait(5000);
            check(!exists(g_real) && !exists(g_shadow), "a save deleted on close is gone");
        } else {
            check(0, "a save deleted on close opens");
        }
    }

    printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
