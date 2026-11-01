/*
 * xst_host.c - the soundtrack library on the emulated disk.
 * See xbox_soundtrack.h.
 *
 * The title's library opens two kinds of path, both under
 * \Device\Harddisk0\Partition1\TDATA\FFFE0000\MUSIC\ :
 *
 *   ST.DB                     the database, read block by block
 *   <%04x>\<%08x>.WMA         a song, only opened (the handle then goes to
 *                             the decoder, which the port answers)
 *
 * Both are sent (xbox_PathRedirectHook) to one generated file,
 * TDATA\FFFE0000\MUSIC.HOST\ST.DB in the save folder: the database for the
 * first, any file that opens for the second. A MUSIC folder the user copied
 * from a console is not touched, and is what the title reads again once the
 * folder setting is cleared.
 */
#include "xbox_soundtrack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "kernel.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */
#endif

#define LOGP "[SOUNDTRACK] "

static const char MUSIC_DIR[]  = "\\Device\\Harddisk0\\Partition1\\TDATA\\FFFE0000\\MUSIC\\";
static const char HOST_DIR[]   = "\\Device\\Harddisk0\\Partition1\\TDATA\\FFFE0000\\MUSIC.HOST";
static const char HOST_DB[]    = "\\Device\\Harddisk0\\Partition1\\TDATA\\FFFE0000\\MUSIC.HOST\\ST.DB";

static xst_library *s_lib;
static volatile int s_active;
static int          s_hooked;
static char         s_dir[1024];          /* the folder in use */
static const char *(*s_prev_hook)(const char *xbox_path);

/* The last handles XOpenSoundtrackSong returned. The title holds one or two
 * at a time; a handle value the kernel reuses is simply noted again. */
#define OPEN_SLOTS 16
static struct { uint32_t handle, song; } s_open[OPEN_SLOTS];
static unsigned s_open_next;
static volatile LONG s_open_lock;

static void open_lock(void)   { while (InterlockedExchange(&s_open_lock, 1)) ; }
static void open_unlock(void) { InterlockedExchange(&s_open_lock, 0); }

static int prefix_ci(const char *s, const char *prefix)
{
    int i;
    for (i = 0; prefix[i]; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    return i;
}

static const char *redirect(const char *xbox_path)
{
    if (s_prev_hook) {
        const char *to = s_prev_hook(xbox_path);
        if (to)
            return to;
    }
    if (s_active && xbox_path && prefix_ci(xbox_path, MUSIC_DIR))
        return HOST_DB;
    return NULL;
}

static int write_db(const uint8_t *db, size_t size)
{
    xbox_host_char dir[MAX_PATH], file[MAX_PATH];
    FILE *f;
    size_t i;
    int ok;

    if (!xbox_translate_path(HOST_DIR, dir, MAX_PATH)
        || !xbox_translate_path(HOST_DB, file, MAX_PATH))
        return 0;
    /* The folder and the one above it; TDATA itself the path layer makes. */
    for (i = 1; dir[i]; i++) {
        if (dir[i] != '/' && dir[i] != '\\')
            continue;
        {
            xbox_host_char c = dir[i];
            dir[i] = 0;
#if defined(_WIN32)
            _wmkdir(dir);
#else
            mkdir(dir, 0755);
#endif
            dir[i] = c;
        }
    }
    /* Written beside and moved over: the title may be reading the old one
     * (it opens the database at every song and every menu step). */
#if defined(_WIN32)
    {
        WCHAR tmp[MAX_PATH + 8];
        int tries;
        _wmkdir(dir);
        _snwprintf(tmp, MAX_PATH + 8, L"%ls.new", file);
        f = _wfopen(tmp, L"wb");
        if (!f)
            return 0;
        ok = fwrite(db, 1, size, f) == size;
        if (fclose(f) != 0 || !ok)
            return 0;
        for (tries = 0; tries < 50; tries++) {
            if (MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING))
                return 1;
            Sleep(20);
        }
        return 0;
    }
#else
    {
        char tmp[MAX_PATH + 8];
        mkdir(dir, 0755);
        snprintf(tmp, sizeof tmp, "%s.new", file);
        f = fopen(tmp, "wb");
        if (!f)
            return 0;
        ok = fwrite(db, 1, size, f) == size;
        if (fclose(f) != 0 || !ok)
            return 0;
        return rename(tmp, file) == 0;
    }
#endif
}

static void log_library(const xst_library *lib, const char *music_dir)
{
    int i;

    fprintf(stderr, LOGP "%s: %d soundtrack%s, %d song%s\n", music_dir,
            lib->n_st, lib->n_st == 1 ? "" : "s", lib->n_songs, lib->n_songs == 1 ? "" : "s");
    for (i = 0; i < lib->n_st && i < 12; i++) {
        char name[XST_NAME_UNITS];
        int k;
        for (k = 0; k < XST_NAME_UNITS; k++)
            name[k] = lib->st[i].name[k] < 0x80 ? (char)lib->st[i].name[k] : '?';
        name[XST_NAME_UNITS - 1] = 0;
        fprintf(stderr, LOGP "  %2u \"%s\": %d song%s, %u:%02u\n", (unsigned)lib->st[i].id, name,
                lib->st[i].count, lib->st[i].count == 1 ? "" : "s",
                (unsigned)(lib->st[i].ms / 60000), (unsigned)(lib->st[i].ms / 1000 % 60));
    }
    if (lib->dropped_folders || lib->dropped_songs)
        fprintf(stderr, LOGP "left out: %d folder%s past the %d soundtracks, %d song%s past "
                "%d in a folder\n",
                lib->dropped_folders, lib->dropped_folders == 1 ? "" : "s", XST_MAX_SOUNDTRACKS,
                lib->dropped_songs, lib->dropped_songs == 1 ? "" : "s", XST_MAX_SONGS);
    if (lib->budget_hit)
        fprintf(stderr, LOGP "the scan stopped after %d folder entries: this is a folder of songs, or a "
                "folder of folders of songs, not a whole disk; what was read so far is used\n",
                XST_SCAN_BUDGET);
    /* Every file that was not taken, by name, with the reason. */
    for (i = 0; i < lib->n_left_out; i++)
        fprintf(stderr, LOGP "left out: %s: %s\n", lib->left_out[i].path, lib->left_out[i].why);
    if (lib->bad_files > lib->n_left_out)
        fprintf(stderr, LOGP "left out: %d more unreadable file%s\n",
                lib->bad_files - lib->n_left_out, lib->bad_files - lib->n_left_out == 1 ? "" : "s");
}

/* `lib` (scanned from `music_dir`) becomes the library in use: its database
 * on the disk, the redirect on. NULL or an empty one: none in use, the
 * title's paths its own again. Returns the songs now offered. Takes `lib`. */
static volatile int s_stopped;          /* the last scan stopped at the budget */
static volatile int s_unreadable;       /* the last folder asked for could not be read */

/* One use_library at a time: the scan's thread and, for a folder held back,
 * the title's (xbox_SoundtrackApplyPending) both write the database. */
static volatile LONG s_use_lock;

static int use_library_locked(xst_library *lib, const char *music_dir);

static int use_library(xst_library *lib, const char *music_dir)
{
    int r;
    while (InterlockedExchange(&s_use_lock, 1))
        ;
    r = use_library_locked(lib, music_dir);
    InterlockedExchange(&s_use_lock, 0);
    return r;
}

static int use_library_locked(xst_library *lib, const char *music_dir)
{
    xst_library *old;
    uint8_t *db = NULL;
    size_t size;

    s_stopped = lib ? lib->budget_hit : 0;
    if (lib && lib->n_songs == 0) {
        if (lib->budget_hit)
            log_library(lib, music_dir);        /* says where it stopped */
        fprintf(stderr, LOGP "%s: no mp3, flac, ogg or wav file found; custom music is off\n",
                music_dir);
        xst_free(lib);
        lib = NULL;
    }
    if (lib) {
        size = xst_db_size(lib);
        db = (uint8_t *)malloc(size);
        if (!db || xst_db_build(lib, db, size) != size || !write_db(db, size)) {
            XR_ERROR( LOGP "cannot write the soundtrack database; custom music is off\n");
            xst_free(lib);
            lib = NULL;
        }
        free(db);
    }

    open_lock();
    old = s_lib;
    s_lib = lib;
    s_active = lib != NULL;
    snprintf(s_dir, sizeof s_dir, "%s", lib ? music_dir : "");
    if (lib && !s_hooked) {
        s_hooked = 1;
        s_prev_hook = xbox_PathRedirectHook;
        xbox_PathRedirectHook = redirect;
    }
    open_unlock();
    xst_free(old);

    if (lib)
        log_library(lib, music_dir);
    return lib ? lib->n_songs : 0;
}

int xbox_SoundtrackInit(const char *music_dir)
{
    xst_library *lib;

    if (s_active || !music_dir || !*music_dir)
        return s_active ? s_lib->n_songs : 0;
    lib = xst_scan(music_dir);
    if (!lib) {
        XR_ERROR( LOGP "%s: cannot read the folder; custom music is off\n", music_dir);
        return 0;
    }
    return use_library(lib, music_dir);
}

int xbox_SoundtrackActive(void)
{
    return s_active;
}

const xst_library *xbox_SoundtrackLibrary(void)
{
    return s_lib;
}

void xbox_SoundtrackInfo(char *dir, size_t cap, int *soundtracks, int *songs)
{
    open_lock();
    if (dir && cap)
        snprintf(dir, cap, "%s", s_dir);
    if (soundtracks)
        *soundtracks = s_lib ? s_lib->n_st : 0;
    if (songs)
        *songs = s_lib ? s_lib->n_songs : 0;
    open_unlock();
}

int xbox_SoundtrackNameOf(int i, char *out, size_t cap)
{
    int r;
    open_lock();
    r = xst_soundtrack_name(s_lib, i, out, cap);
    open_unlock();
    return r;
}

int xbox_SoundtrackIndexOfName(const char *utf8)
{
    int r;
    open_lock();
    r = xst_soundtrack_find(s_lib, utf8);
    open_unlock();
    return r;
}

int xbox_SoundtrackLeftOut(int *unreadable, int *songs_past_limit, int *folders_past_limit)
{
    int a = 0, b = 0, c = 0;

    open_lock();
    if (s_lib) {
        a = s_lib->bad_files;
        b = s_lib->dropped_songs;
        c = s_lib->dropped_folders;
    }
    open_unlock();
    if (unreadable) *unreadable = a;
    if (songs_past_limit) *songs_past_limit = b;
    if (folders_past_limit) *folders_past_limit = c;
    return a + b + c;
}

int xbox_SoundtrackScanStopped(void)
{
    return s_stopped;
}

int xbox_SoundtrackFolderUnreadable(void)
{
    return s_unreadable;
}

int xbox_SoundtrackLeftOutFile(int i, char *name, size_t cap, const char **why)
{
    int ok = 0;

    open_lock();
    if (s_lib && i >= 0 && i < s_lib->n_left_out) {
        const char *p = s_lib->left_out[i].path, *leaf = p, *c;
        for (c = p; *c; c++)
            if (*c == '/' || *c == '\\')
                leaf = c + 1;
        if (name && cap)
            snprintf(name, cap, "%s", leaf);
        if (why)
            *why = s_lib->left_out[i].why;
        ok = 1;
    }
    open_unlock();
    return ok;
}

/* ── a change while the title runs ─────────────────────────────────── */

static char          s_want[1024];
static volatile LONG s_want_gen;
static volatile LONG s_scanning;
static xst_progress  s_progress;
static void        (*s_done)(int soundtracks, int songs);
static int         (*s_gate)(void);
/* A folder read while the gate said no: put in place by
 * xbox_SoundtrackApplyPending (NULL lib: the folder is off or unreadable). */
static int          s_pend_set;
static xst_library *s_pend_lib;
static char         s_pend_dir[1024];
static int          s_pend_unreadable;

/* Drops a held-back folder; under open_lock. Returns it to be freed. */
static xst_library *pend_take(void)
{
    xst_library *lib = s_pend_lib;
    s_pend_lib = NULL;
    s_pend_set = 0;
    s_pend_dir[0] = 0;
    return lib;
}

int xbox_SoundtrackPending(char *dir, size_t cap, int *soundtracks)
{
    int set;
    open_lock();
    set = s_pend_set;
    if (dir && cap)
        snprintf(dir, cap, "%s", set ? s_pend_dir : "");
    if (soundtracks)
        *soundtracks = set && s_pend_lib && s_pend_lib->n_songs ? s_pend_lib->n_st : 0;
    open_unlock();
    return set;
}

int xbox_SoundtrackApplyPending(int *soundtracks, int *songs)
{
    xst_library *lib;
    char dir[1024];
    int set, unreadable;

    open_lock();
    set = s_pend_set;
    unreadable = s_pend_unreadable;
    snprintf(dir, sizeof dir, "%s", s_pend_dir);
    lib = s_pend_lib;
    s_pend_lib = NULL;
    s_pend_set = 0;
    s_pend_dir[0] = 0;
    open_unlock();
    if (!set)
        return 0;
    s_unreadable = unreadable;
    use_library(lib, dir);
    fprintf(stderr, LOGP "%s: the folder held back is in use now\n", dir[0] ? dir : "(none)");
    xbox_SoundtrackInfo(NULL, 0, soundtracks, songs);
    return 1;
}

void xbox_SoundtrackSetSwapGate(int (*may_swap)(void))
{
    s_gate = may_swap;
}

static DWORD WINAPI scan_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        char dir[1024];
        void (*done)(int, int);
        xst_library *lib = NULL;
        LONG gen;
        int n_st = 0, n_songs = 0, last;

        open_lock();
        gen = s_want_gen;
        snprintf(dir, sizeof dir, "%s", s_want);
        done = s_done;
        s_progress.folders = s_progress.files = s_progress.songs = 0;
        s_progress.cancel = 0;
        open_unlock();

        if (dir[0]) {
            lib = xst_scan_progress(dir, &s_progress);
            if (!lib && !s_progress.cancel)
                XR_ERROR( LOGP "%s: cannot read the folder; custom music is off\n", dir);
            s_unreadable = !lib && !s_progress.cancel;
        } else {
            s_unreadable = 0;
            fprintf(stderr, LOGP "custom music is off\n");
        }

        open_lock();
        last = gen == s_want_gen;
        open_unlock();
        if (!last) {                      /* asked again meanwhile */
            xst_free(lib);
            continue;
        }
        if (s_gate && !s_gate()) {
            /* The title holds an index into the library in use: not
             * swapped under it (the port's check, now, not when the folder
             * was chosen: the title may have chosen a soundtrack since).
             * Held back, read, for xbox_SoundtrackApplyPending. */
            xst_library *drop;
            open_lock();
            drop = pend_take();
            if (gen == s_want_gen) {
                s_pend_set = 1;
                s_pend_lib = lib;
                s_pend_unreadable = !lib && dir[0];
                snprintf(s_pend_dir, sizeof s_pend_dir, "%s", dir);
                lib = NULL;
            }
            open_unlock();
            xst_free(drop);
            xst_free(lib);
            fprintf(stderr, LOGP "%s: read; held back while a soundtrack of the library in use is "
                    "the title's music\n", dir[0] ? dir : "(none)");
            if (done)
                done(-1, 0);
        } else {
            use_library(lib, dir);
            xbox_SoundtrackInfo(NULL, 0, &n_st, &n_songs);
            if (done)
                done(n_st, n_songs);
        }
        open_lock();
        last = gen == s_want_gen;
        if (last)
            s_scanning = 0;
        open_unlock();
        if (last)
            return 0;
    }
}

int xbox_SoundtrackSetFolder(const char *music_dir, void (*done)(int soundtracks, int songs))
{
    int start;

    xst_library *drop;

    open_lock();
    drop = pend_take();                   /* a folder held back is replaced by this one */
    snprintf(s_want, sizeof s_want, "%s", music_dir ? music_dir : "");
    s_done = done;
    s_want_gen++;
    s_progress.cancel = 1;
    start = !s_scanning;
    if (start)
        s_scanning = 1;
    open_unlock();
    xst_free(drop);
    if (start) {
        HANDLE t = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
        if (!t) {
            open_lock();
            s_scanning = 0;
            open_unlock();
            return 0;
        }
        CloseHandle(t);
    }
    return 1;
}

int xbox_SoundtrackScanning(int *folders, int *files, int *songs)
{
    if (folders) *folders = s_progress.folders;
    if (files)   *files = s_progress.files;
    if (songs)   *songs = s_progress.songs;
    return s_scanning != 0;
}

void xbox_SoundtrackNoteOpen(uint32_t handle, uint32_t song_id)
{
    unsigned i;

    if (!s_active || handle == 0xFFFFFFFFu || handle == 0)
        return;
    open_lock();
    for (i = 0; i < OPEN_SLOTS; i++)
        if (s_open[i].handle == handle)
            break;
    if (i == OPEN_SLOTS)
        i = s_open_next++ % OPEN_SLOTS;
    s_open[i].handle = handle;
    s_open[i].song = song_id;
    open_unlock();
}

xst_stream *xbox_SoundtrackOpenHandle(uint32_t handle, int *ours)
{
    char path[1024] = "";
    uint32_t id = 0;
    xst_stream *s;
    unsigned i, rate = 0, ch = 0;
    int found = 0;

    if (ours)
        *ours = 0;
    if (!s_active)
        return NULL;
    open_lock();
    for (i = 0; i < OPEN_SLOTS; i++)
        if (s_open[i].handle == handle && handle) {
            /* A song noted before the folder changed may not be in the new
             * library: it is still ours, and over at once. */
            const xst_song *song = xst_song_by_id(s_lib, s_open[i].song);
            found = 1;
            id = s_open[i].song;
            if (song)
                snprintf(path, sizeof path, "%s", song->path);
            break;
        }
    open_unlock();
    if (!found)
        return NULL;
    if (ours)
        *ours = 1;
    s = path[0] ? xst_stream_open(path) : NULL;
    if (!s) {
        XR_ERROR( LOGP "cannot play song %08X %s\n", (unsigned)id, path);
        return NULL;
    }
    xst_stream_format(s, &rate, &ch);
    fprintf(stderr, LOGP "song %08X: %s (%u Hz, %u ch)\n", (unsigned)id, path, rate, ch);
    return s;
}
