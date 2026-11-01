/*
 * Where a read with an explicit offset leaves the file position (BO-221).
 *
 * NT moves CurrentByteOffset of a synchronous file object only when the
 * request succeeds: a read at or past the end (STATUS_END_OF_FILE) leaves
 * it where it was, and one that delivers leaves it after what it read. The
 * POSIX backend did lseek(offset) and then read(), so a read at a garbage
 * offset that hit the end left the descriptor there, and every sequential
 * read after it on the handle got 0 bytes: a hung loading screen on the
 * Deck, sound\Ident2.awd (53248 bytes) at position 0x282844A6.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/stat.h>
#endif

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define FILE_SIZE 53248u     /* sound\Ident2.awd */

static int failures;

static void check(int ok, const char *what, long long got)
{
    printf("  %-64s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got %lld (0x%llX)", got, (unsigned long long)got);
    putchar('\n');
    if (!ok) failures++;
}

static unsigned char byte_at(unsigned i) { return (unsigned char)(i * 7u + (i >> 11)); }

static HANDLE open_sync(const char *path)
{
    XBOX_ANSI_STRING s;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE h = NULL;
    s.Length = (USHORT)strlen(path);
    s.MaximumLength = (USHORT)(s.Length + 1);
    s.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &s;
    oa.Attributes = 0x40;
    /* GENERIC_READ|SYNCHRONIZE, FILE_OPEN, the title's bank options 0x6C:
     * non-directory, synchronous non-alert, no buffering, sequential. */
    if (!NT_SUCCESS(xbox_NtCreateFile(&h, 0x80100080u, &oa, &ios, NULL, 0, 1,
                                      XBOX_FILE_OPEN, 0x6C)))
        return NULL;
    return h;
}

static NTSTATUS rd(HANDLE h, void *buf, ULONG n, long long at, ULONG *got)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER off;
    NTSTATUS st;
    memset(&ios, 0, sizeof ios);
    off.QuadPart = at;
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, n, at == -1 ? NULL : &off);
    *got = (ULONG)ios.Information;
    return st;
}

static int bytes_from(const unsigned char *b, unsigned at, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (b[i] != byte_at(at + i)) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    char root[512], dir[600], file[700];
    static unsigned char buf[4096];
    unsigned i;
    ULONG got;
    NTSTATUS st;
    HANDLE h;
    FILE *f;

    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        unsigned n = 0;
        GetTempPathA(MAX_PATH, tmp);
        do snprintf(root, sizeof root, "%sfilepos-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
        snprintf(dir, sizeof dir, "%s\\sound", root);
        CreateDirectoryA(dir, NULL);
        snprintf(file, sizeof file, "%s\\Ident2.awd", dir);
    }
#else
    {
        char tmpl[] = "/tmp/fileposXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(dir, sizeof dir, "%s/sound", root);
        mkdir(dir, 0755);
        snprintf(file, sizeof file, "%s/Ident2.awd", dir);
    }
#endif
    f = fopen(file, "wb");
    if (!f) return 2;
    for (i = 0; i < FILE_SIZE; i++) fputc(byte_at(i), f);
    fclose(f);
    xbox_path_init(root, root);

    h = open_sync("D:\\sound\\Ident2.awd");
    check(h != NULL, "open D:\\sound\\Ident2.awd", 0);
    if (!h) return 1;

    printf("explicit-offset read past the end\n");
    st = rd(h, buf, 2048, -1, &got);
    check(NT_SUCCESS(st) && got == 2048 && bytes_from(buf, 0, 2048), "sequential read of the first sector", (long long)got);
    st = rd(h, buf, 2048, 0x282844A6LL, &got);
    check((uint32_t)st == 0xC0000011u && got == 0, "read at 0x282844A6: STATUS_END_OF_FILE, 0 bytes", (long long)(uint32_t)st);
    check(xbox_FileHandleTell(h) == 2048, "... the position stays at 2048", xbox_FileHandleTell(h));
    st = rd(h, buf, 2048, -1, &got);
    check(NT_SUCCESS(st) && got == 2048 && bytes_from(buf, 2048, 2048),
          "next sequential read gets the second sector", (long long)got);

    printf("explicit-offset read at the end exactly\n");
    st = rd(h, buf, 2048, FILE_SIZE, &got);
    check((uint32_t)st == 0xC0000011u && got == 0, "read at the size: STATUS_END_OF_FILE", (long long)(uint32_t)st);
    check(xbox_FileHandleTell(h) == 4096, "... the position stays at 4096", xbox_FileHandleTell(h));

    printf("explicit-offset read that delivers\n");
    st = rd(h, buf, 2048, 8192, &got);
    check(NT_SUCCESS(st) && got == 2048 && bytes_from(buf, 8192, 2048), "read 2048 at 8192", (long long)got);
    check(xbox_FileHandleTell(h) == 10240, "... the position moves to 10240 (sync handle)", xbox_FileHandleTell(h));
    st = rd(h, buf, 4096, FILE_SIZE - 1024, &got);
    check(NT_SUCCESS(st) && got == 1024 && bytes_from(buf, FILE_SIZE - 1024, 1024), "short read at the tail: 1024 bytes", (long long)got);
    check(xbox_FileHandleTell(h) == FILE_SIZE, "... the position moves to the end", xbox_FileHandleTell(h));

    printf("FILE_USE_FILE_POINTER_POSITION reads at the position\n");
    st = rd(h, buf, 2048, 0, &got);           /* back to 2048 */
    {
        XBOX_IO_STATUS_BLOCK ios;
        LARGE_INTEGER off;
        memset(&ios, 0, sizeof ios);
        off.LowPart = 0xFFFFFFFEu;            /* {-2, -1} */
        off.HighPart = -1;
        st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, 2048, &off);
        check(NT_SUCCESS(st) && ios.Information == 2048 && bytes_from(buf, 2048, 2048),
              "{-2,-1} reads the sector after the last read", (long long)ios.Information);
    }

    xbox_NtClose(h);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
