/* soundtrack_close: a song file the decoder cannot open is closed once.
 *
 *   soundtrack_close_test <work dir>
 *
 * stb_vorbis_open_file(f, close_on_free = 1, ...) closes f itself when the
 * stream does not open. ogg_open closed it again: on glibc the second
 * fclose reads the freed FILE and closes its descriptor number a second
 * time, which by then can be a file another thread just opened (BO-221).
 * xst_decode.c is built here with fopen/fclose counted (close_track.h). */
#include "xbox_soundtrack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES 64
static FILE *s_open[MAX_FILES];
static int s_doubles, s_opens, s_closes;

FILE *track_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    int i;
    if (f) {
        s_opens++;
        for (i = 0; i < MAX_FILES; i++)
            if (!s_open[i]) { s_open[i] = f; break; }
    }
    return f;
}

int track_fclose(FILE *f)
{
    int i;
    for (i = 0; i < MAX_FILES; i++)
        if (s_open[i] == f) {
            s_open[i] = NULL;
            s_closes++;
            return fclose(f);
        }
    s_doubles++;          /* not open: a second close of the same FILE */
    return EOF;           /* not passed on: that would be the bug itself */
}

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static void write_bytes(const char *path, const char *bytes, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { CHECK("write", 0); return; }
    fwrite(bytes, 1, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    char path[1024];
    static const char junk[] = "OggS\0\x02\0\0\0\0\0\0\0\0this is not a vorbis stream at all......";
    uint32_t ms = 0;
    char title[64];

    if (argc < 2) {
        printf("usage: soundtrack_close_test <work dir>\n");
        return 2;
    }
    snprintf(path, sizeof path, "%s/broken.ogg", argv[1]);
    write_bytes(path, junk, sizeof junk);

    CHECK("a broken ogg does not open as a stream", xst_stream_open(path) == NULL);
    CHECK("a broken ogg is not a song", !xst_probe(path, &ms, title, sizeof title));
    printf("opens %d, closes %d, second closes %d\n", s_opens, s_closes, s_doubles);
    CHECK("each file closed once", s_doubles == 0);
    CHECK("each file closed", s_opens == s_closes);
    printf(failures ? "%d FAILED\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
