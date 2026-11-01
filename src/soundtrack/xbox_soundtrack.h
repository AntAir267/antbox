/*
 * xbox_soundtrack.h - the user's music folder as Xbox custom soundtracks.
 *
 * A title that links the XDK soundtrack library (XFindFirstSoundtrack,
 * XGetSoundtrackSongInfo, XOpenSoundtrackSong) reads the dashboard's music
 * database, \Device\Harddisk0\Partition1\TDATA\FFFE0000\MUSIC\ST.DB, and
 * plays the songs it lists (MUSIC\<soundtrack>\<song id>.WMA) through the
 * XDK's WMA decoder. Here the database is made from a folder of the user's
 * own files and the songs are decoded by the host:
 *
 *   xst_library.c   the folder scan and the ST.DB image (no decoder needed)
 *   xst_decode.c    a song as 44.1 kHz 16-bit stereo, the format the
 *                   dashboard ripped to, from mp3 / flac / ogg / wav
 *   xst_host.c      the runtime side: the database on the emulated disk and
 *                   which song an opened handle is
 *
 * Opt-in: nothing here runs until a host calls xbox_SoundtrackInit with a
 * folder. The title-side hooks (which guest functions open a song and make
 * its decoder) belong to the port: see docs/runtime/soundtrack.md.
 *
 * The ST.DB layout is the one the XDK library in the title reads (512-byte
 * blocks: a header, 100 soundtrack blocks, then song-group blocks of six
 * songs); it is also described at https://xboxdevwiki.net/Soundtracks.
 */
#ifndef XBOX_SOUNDTRACK_H
#define XBOX_SOUNDTRACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The database's own limits: 100 soundtrack blocks, 84 song groups of six a
 * soundtrack. 500 songs is what the dashboard stopped at. */
#define XST_MAX_SOUNDTRACKS   100
#define XST_MAX_SONGS         500
#define XST_SONGS_PER_GROUP   6
#define XST_MAX_GROUPS        84
#define XST_NAME_UNITS        32          /* UTF-16 units, the NUL included */
#define XST_BLOCK             512
#define XST_MAX_DEPTH         3           /* folders below the chosen one: a soundtrack's, and two in it */
#define XST_SCAN_BUDGET       20000       /* folder entries a scan reads at most */
#define XST_LEFT_OUT_KEPT     32          /* unreadable files remembered by name */

/* The stream every song is delivered as. */
#define XST_RATE              44100
#define XST_CHANNELS          2

typedef struct xst_song {
    char    *path;                        /* host path, UTF-8 */
    uint16_t name[XST_NAME_UNITS];
    uint32_t ms;                          /* length, 0 when unknown */
    uint32_t id;                          /* (soundtrack id << 16) | index */
} xst_song;

typedef struct xst_soundtrack {
    uint16_t name[XST_NAME_UNITS];
    uint32_t id;                          /* 1.. */
    int      first, count;                /* its songs in xst_library.songs */
    uint32_t ms;                          /* sum of its songs */
} xst_soundtrack;

typedef struct xst_library {
    xst_soundtrack st[XST_MAX_SOUNDTRACKS];
    int       n_st;
    xst_song *songs;
    int       n_songs, cap_songs;
    /* What the limits and unreadable files left out; of the unreadable
     * ones the first XST_LEFT_OUT_KEPT by name, with the reason. */
    int       dropped_folders, dropped_songs, bad_files;
    struct { char *path; const char *why; } left_out[XST_LEFT_OUT_KEPT];
    int       n_left_out;
    int       budget_hit;                 /* stopped at XST_SCAN_BUDGET entries */
} xst_library;

/* ── xst_library.c ─────────────────────────────────────────────────── */

/* Scan `dir`: a folder of songs, or a folder of folders, each one a
 * soundtrack. The songs in `dir` itself are one soundtrack; each folder
 * directly in it is one more, with the songs in it and in its folders two
 * levels further down (albums, discs) flattened into it in natural order:
 * those folders are not soundtracks of their own. Nothing deeper is read,
 * and a folder that is a link is not followed. Each is named after its
 * folder, its songs named by the title tag, or the file name without the
 * extension. Hidden entries (a leading dot) are skipped. At most
 * XST_SCAN_BUDGET folder entries are read; past that the scan stops with
 * what it has (budget_hit). NULL when `dir` cannot be read; a folder with
 * no playable file gives a library with n_st == 0. */
xst_library *xst_scan(const char *dir);
/* The same, watched: the counts grow while it runs (another thread may read
 * them), and setting `cancel` ends it early with NULL. */
typedef struct xst_progress {
    volatile int folders, files, songs;
    volatile int cancel;
} xst_progress;
xst_library *xst_scan_progress(const char *dir, xst_progress *progress);
void         xst_free(xst_library *lib);

const xst_song *xst_song_by_id(const xst_library *lib, uint32_t id);
/* Soundtrack i's (0-based, in the title's order) name as UTF-8, as the title
 * lists it (cut and folded to Latin-1); 0 when there is none. */
int xst_soundtrack_name(const xst_library *lib, int i, char *out, size_t cap);
/* The index of the soundtrack that has this name (given as UTF-8, folded the
 * same way before the comparison), or -1. The first when two share a name. */
int xst_soundtrack_find(const xst_library *lib, const char *utf8);

/* The ST.DB image: xst_db_size bytes. Returns the bytes written, 0 when
 * `cap` is too small. */
size_t xst_db_size(const xst_library *lib);
size_t xst_db_build(const xst_library *lib, uint8_t *out, size_t cap);

/* Natural, case-insensitive order of two names (digit runs by value). */
int  xst_natural_cmp(const char *a, const char *b);
/* UTF-8 to a fixed UTF-16 name, cut at XST_NAME_UNITS - 1 units and kept
 * to Latin-1, which is as far as a title's menu font goes: decomposed
 * letters are composed, Latin Extended-A is folded to the nearest letter
 * ("ő" to "ö"), anything else above it is '?'. */
void xst_name_from_utf8(uint16_t out[XST_NAME_UNITS], const char *utf8);
/* UTF-8 as a screen with a font wants it: a letter followed by a combining
 * mark (how macOS spells a file name: "o" + U+030B) becomes the one
 * character there is for it in Latin-1 or Latin Extended-A ("ő"). The rest
 * is copied; a broken sequence becomes U+FFFD. Never longer than the input;
 * cut at cap - 1 bytes, on a character. */
void xst_utf8_compose(char *out, size_t cap, const char *utf8);
/* 1 when the file name has an extension xst_decode.c plays. */
int  xst_is_playable_name(const char *name);

/* ── xst_decode.c ──────────────────────────────────────────────────── */

/* A file's length (ms, 0 unknown) and title tag ("" when it has none)
 * without decoding it. 0 when it is not a file this can play. */
int xst_probe(const char *path, uint32_t *ms, char *title, size_t title_cap);
/* The same, and for a file that is not: why, in a few words for a log or a
 * settings screen (a constant string). */
int xst_probe_why(const char *path, uint32_t *ms, char *title, size_t title_cap, const char **why);

typedef struct xst_stream xst_stream;

/* A song as XST_RATE stereo 16-bit frames, resampled and folded as needed.
 * NULL when the file cannot be opened or is not playable. */
xst_stream *xst_stream_open(const char *path);
/* Up to `frames` frames; fewer only at the end of the song, then 0. */
size_t      xst_stream_read(xst_stream *s, int16_t *out, size_t frames);
void        xst_stream_close(xst_stream *s);
/* The file's own rate and channel count (tests, the log). */
void        xst_stream_format(const xst_stream *s, unsigned *rate, unsigned *channels);

/* ── xst_browse.c ──────────────────────────────────────────────────── */

/*
 * A folder browser's model, for a settings screen that has no file dialog
 * (the in-game overlay, driven with a pad): where to start, a folder's
 * folders, and how much music is in each. No drawing here.
 *
 * It opens on the places: the current choice and the folder it is in (in
 * the current choice the cursor is on ".."), the home folder, Music,
 * Downloads, and what is mounted (/Volumes on macOS; /run/media, /media and
 * /mnt on Linux, the Steam Deck's SD card among them; the drive letters on
 * Windows; shared storage on Android). In a folder the rows are "use this
 * folder", "..", and its folders in natural order. The number of playable
 * files in and below each row (XST_MAX_DEPTH deep, as a scan looks) is
 * counted on a thread of the browser's own and shows when it is known; a
 * count stops at XSB_COUNT_BUDGET entries and says so (`more`).
 */
#define XSB_MAX_ROWS      1024
#define XSB_COUNT_BUDGET  20000
#define XSB_COUNT_MS      3000            /* and as long as a row's count may take */

typedef enum { XSB_USE, XSB_OFF, XSB_UP, XSB_PLACE, XSB_DIR } xsb_kind;
typedef struct xsb_row {
    xsb_kind kind;
    char label[200];
    char path[1024];
    int  songs;                           /* -1: not counted yet */
    int  more;                            /* the count stopped at the budget */
} xsb_row;
typedef enum { XSB_NOTHING, XSB_MOVED, XSB_CHOSEN, XSB_TURNED_OFF, XSB_CLOSED } xsb_result;

/* Open on the places; `current` is the folder in use ("" or NULL: none). */
void xsb_open(const char *current);
/* The same browser for another purpose than the music folder (a save to
 * import): its own words, a place of its own at the top, and in the place
 * of the song counts a yes or no of its own for each folder (`mark`, called
 * on the browser's thread; a row's `songs` is then 1 or 0). NULL fields:
 * "Use this folder", no "turn off" row, no extra place, song counts. */
typedef struct xsb_options {
    const char *current;
    const char *use_label, *off_label;
    const char *top_label, *top_path;
    int (*mark)(const char *dir);
    const char *start_in;                 /* open in this folder, not on the places */
    int newest_first;                     /* folders in reverse name order (dated ones) */
} xsb_options;
void xsb_open_with(const xsb_options *o);
void xsb_close(void);
int  xsb_is_open(void);
int  xsb_rows(void);
int  xsb_row_at(int i, xsb_row *out);     /* 1 when there is such a row */
/* The folder being shown, "" on the places. */
void xsb_where(char *out, size_t cap);
/* A row is picked: a folder or place is entered (XSB_MOVED), "use this
 * folder" gives its path in `chosen` (XSB_CHOSEN), "turn off" XSB_TURNED_OFF.
 * *select is the row to put the cursor on after a move. */
xsb_result xsb_activate(int i, char *chosen, size_t cap, int *select);
/* One level up; from a place's own folder back to the places; from the
 * places XSB_CLOSED. Entered from the current choice or the folder it is
 * in, it goes on up to the top of the disk, then to the places. */
xsb_result xsb_back(int *select);
/* Tests: the places come from these instead of the system's (home folder,
 * NULL-terminated media roots). NULL, NULL: the system's again. */
void xsb_set_roots(const char *home, const char *const *media_roots);

/* BO-289. A close never waits for the counting thread: one held by a share
 * that does not answer leaves on its own later, and its count is dropped.
 * For a `mark` (it runs on that thread): xsb_stopping() is 1 once the
 * browser closed or moved to another list, so a long look can give up;
 * xsb_slow_volume(dir) is 1 for a folder on a network share (macOS smbfs,
 * nfs, afpfs, webdav, macFUSE; Linux NFS, SMB/CIFS, sshfs, rclone, gvfs;
 * a Windows network drive), which is counted or checked by its own
 * entries and not walked. A `mark` may return -1: not known (shown as a
 * row with `more` and songs 0). */
int  xsb_stopping(void);
int  xsb_slow_volume(const char *dir);
/* Tests: a hook called at each folder a count walks into (on the counting
 * thread), and a part of a path that makes a folder count as on a slow
 * volume; NULL, NULL: none. xsb_workers() is the number of counting
 * threads not yet gone. */
void xsb_set_test_hooks(void (*dir_hook)(const char *dir), const char *slow_part);
int  xsb_workers(void);

/* ── xst_host.c ────────────────────────────────────────────────────── */

/* Scan `music_dir`, write the database to the emulated disk (beside, not
 * over, a MUSIC folder the user copied from a console) and send the title's
 * MUSIC paths to it (xbox_PathRedirectHook; a hook already installed keeps
 * being asked first). Returns the number of songs; 0 leaves everything as it
 * was (no folder, nothing playable in it). Call once, after xbox_path_init
 * and before the title starts. */
int  xbox_SoundtrackInit(const char *music_dir);
int  xbox_SoundtrackActive(void);
const xst_library *xbox_SoundtrackLibrary(void);

/* A change while the title runs: scan `music_dir` on a thread of its own
 * and, when the scan is through, put its database in place of the one in
 * use ("" or NULL: none, the title's MUSIC paths are its own again). A song
 * that is playing plays on; the next one comes from the new folder. `done`
 * (may be NULL) is called on that thread once the change is in, with the
 * number of soundtracks and songs now offered (0, 0: off or nothing found).
 * A change asked for while one runs replaces it. Returns 0 when the thread
 * could not be started. */
int  xbox_SoundtrackSetFolder(const char *music_dir, void (*done)(int soundtracks, int songs));

/* Optional, the port's: asked on the scan's thread right before a folder it
 * has read replaces the library in use. 0 keeps the library in use (the
 * title holds an index into it: a soundtrack of it is chosen) and holds the
 * new one back, read and ready; `done` is then called with soundtracks = -1,
 * and the port puts it in place with xbox_SoundtrackApplyPending at a moment
 * the title holds no index into the old one (BO-243). NULL: always
 * replaced. */
void xbox_SoundtrackSetSwapGate(int (*may_swap)(void));
/* 1 while a folder read is held back by the gate (the folder, when `dir`;
 * its number of soundtracks, when `soundtracks`). */
int  xbox_SoundtrackPending(char *dir, size_t cap, int *soundtracks);
/* The folder held back by the gate replaces the library in use now (the
 * caller's thread). 1 when it did, with its counts (each may be NULL); 0
 * when nothing was held back. A new xbox_SoundtrackSetFolder drops it. */
int  xbox_SoundtrackApplyPending(int *soundtracks, int *songs);
/* 1 while a scan runs; the counts so far (each may be NULL). */
int  xbox_SoundtrackScanning(int *folders, int *files, int *songs);
/* The folder in use ("" when off) and its counts. */
void xbox_SoundtrackInfo(char *dir, size_t cap, int *soundtracks, int *songs);
/* The soundtracks in use, by name (the two above on the library in use,
 * under the lock a swap takes): soundtrack i's name as UTF-8 (0: none), and
 * the index of the one with this name (-1: none). The port remembers a
 * choice by name because the list changes with the folder. */
int  xbox_SoundtrackNameOf(int i, char *out, size_t cap);
int  xbox_SoundtrackIndexOfName(const char *utf8);
/* What the folder in use had that is not offered: files that could not be
 * read, songs past XST_MAX_SONGS in a folder, folders past
 * XST_MAX_SOUNDTRACKS (each may be NULL). Returns their sum. */
int  xbox_SoundtrackLeftOut(int *unreadable, int *songs_past_limit, int *folders_past_limit);
/* 1 when the folder in use was too big to read whole (XST_SCAN_BUDGET
 * entries): what was read is offered. */
int  xbox_SoundtrackScanStopped(void);
/* 1 when the last folder asked for could not be read at all (gone: a card
 * taken out, the folder deleted or renamed, no permission): custom music is
 * off and the title plays its own. */
int  xbox_SoundtrackFolderUnreadable(void);
/* Unreadable file i of those remembered (XST_LEFT_OUT_KEPT): its name
 * without the folder, and why. 0 when there is no such one. */
int  xbox_SoundtrackLeftOutFile(int i, char *name, size_t cap, const char **why);

/* The port's hook on the title's XOpenSoundtrackSong: `handle` (the guest's
 * 32-bit handle value) is the song `song_id` until it is noted again. */
void xbox_SoundtrackNoteOpen(uint32_t handle, uint32_t song_id);
/* The port's hook where the title makes the decoder for a handle: the song's
 * stream, or NULL when the handle is not one of ours (the title's own
 * decoder then takes it) or the file fails to open (*ours says which). */
xst_stream *xbox_SoundtrackOpenHandle(uint32_t handle, int *ours);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_SOUNDTRACK_H */
