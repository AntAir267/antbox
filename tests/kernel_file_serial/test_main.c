/*
 * Two threads on one synchronous file handle (BO-221).
 *
 * Burnout 3's file thread reads a bank through a handle while its main
 * thread asks the same handle where it is: XAPI SetFilePointer(FILE_CURRENT,
 * 0), which is a position query and then a position set, two kernel calls.
 * On the console the pair does not get split by the file thread's seek and
 * read (one CPU; the title's threads do not preempt each other there), so
 * the set puts back the position it just read and changes nothing. Here
 * the threads run at once, and a set that lands after the other thread's
 * seek and before its read sends that read to the wrong sector: Fe.awd's
 * first sector went to the scratch buffer and its header buffer got the
 * second one (the hang at race start), or a garbage position stuck.
 *
 *   thread A: set position 0, read 4 bytes at the position, expect dword 0
 *   thread B: query the position, set it to what the query said
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

#define ROUNDS 200000
#define DWORDS 4096u

static HANDLE s_h;
static volatile LONG s_stop;
static volatile LONG s_bad, s_errors;

static NTSTATUS set_pos(long long at)
{
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_FILE_POSITION_INFORMATION pi;
    pi.CurrentByteOffset.QuadPart = at;
    return xbox_NtSetInformationFile(s_h, &ios, &pi, sizeof pi, XboxFilePositionInformation);
}

static DWORD WINAPI reader(LPVOID arg)
{
    int i;
    (void)arg;
    for (i = 0; i < ROUNDS; i++) {
        XBOX_IO_STATUS_BLOCK ios;
        uint32_t v = 0xFFFFFFFFu;
        if (!NT_SUCCESS(set_pos(0))) { InterlockedIncrement(&s_errors); continue; }
        memset(&ios, 0, sizeof ios);
        if (!NT_SUCCESS(xbox_NtReadFile(s_h, NULL, NULL, NULL, &ios, &v, 4, NULL)) || ios.Information != 4)
            InterlockedIncrement(&s_errors);
        else if (v != 0)
            InterlockedIncrement(&s_bad);
    }
    InterlockedExchange(&s_stop, 1);
    return 0;
}

static DWORD WINAPI teller(LPVOID arg)
{
    (void)arg;
    while (!s_stop) {
        XBOX_IO_STATUS_BLOCK ios;
        XBOX_FILE_POSITION_INFORMATION pi;
        if (!NT_SUCCESS(xbox_NtQueryInformationFile(s_h, &ios, &pi, sizeof pi, XboxFilePositionInformation)))
            continue;
        set_pos(pi.CurrentByteOffset.QuadPart);
    }
    return 0;
}

int main(int argc, char **argv)
{
    char root[512], file[700];
    XBOX_ANSI_STRING s;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE ta, tb;
    uint32_t i;
    FILE *f;

    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        unsigned n = 0;
        GetTempPathA(MAX_PATH, tmp);
        do snprintf(root, sizeof root, "%sfileserial-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
        snprintf(file, sizeof file, "%s\\bank.awd", root);
    }
#else
    {
        char tmpl[] = "/tmp/fileserialXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(file, sizeof file, "%s/bank.awd", root);
    }
#endif
    f = fopen(file, "wb");
    if (!f) return 2;
    for (i = 0; i < DWORDS; i++) fwrite(&i, 4, 1, f);    /* the dword at 4k is k */
    fclose(f);
    xbox_path_init(root, root);

    s.Buffer = (PCHAR)"D:\\bank.awd";
    s.Length = (USHORT)strlen(s.Buffer);
    s.MaximumLength = (USHORT)(s.Length + 1);
    oa.RootDirectory = NULL;
    oa.ObjectName = &s;
    oa.Attributes = 0x40;
    if (!NT_SUCCESS(xbox_NtCreateFile(&s_h, 0x80100080u, &oa, &ios, NULL, 0, 1, XBOX_FILE_OPEN, 0x6C))) {
        printf("open failed\nFAILED\n");
        return 1;
    }

    ta = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    tb = CreateThread(NULL, 0, teller, NULL, 0, NULL);
    WaitForSingleObject(ta, INFINITE);
    WaitForSingleObject(tb, INFINITE);
    CloseHandle(ta);
    CloseHandle(tb);
    xbox_NtClose(s_h);

    printf("  %d rounds of set 0 + read, against query + set on another thread\n", ROUNDS);
    printf("  reads that got another dword than the one at 0: %ld\n", (long)s_bad);
    printf("  failed calls: %ld\n", (long)s_errors);
    printf("%s\n", s_bad || s_errors ? "FAILED" : "OK");
    return s_bad || s_errors ? 1 : 0;
}
