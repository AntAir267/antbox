/*
 * recomp_utf8_args.c - see recomp_utf8_args.h.
 */
#include "recomp_utf8_args.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

size_t recomp_utf16_to_utf8(const uint16_t *w, char *out, size_t cap)
{
    size_t need = 0, n = 0;
    int full = 0;

    for (; *w; w++) {
        uint32_t c = *w;
        char b[4];
        size_t k;

        if (c >= 0xD800 && c <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[1] - 0xDC00);
            w++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            b[0] = (char)c; k = 1;
        } else if (c < 0x800) {
            b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); k = 2;
        } else if (c < 0x10000) {
            b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[2] = (char)(0x80 | (c & 0x3F)); k = 3;
        } else {
            b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); k = 4;
        }
        need += k;
        if (!full && cap && n + k < cap) {
            memcpy(out + n, b, k);
            n += k;
        } else {
            full = 1;
        }
    }
    if (cap)
        out[n] = 0;
    return need + 1;
}

char **recomp_args_from_utf16(int argc, const uint16_t *const *wargv)
{
    size_t bytes = 0, at;
    char **argv;
    char *text;
    int i;

    if (argc < 0)
        return NULL;
    for (i = 0; i < argc; i++)
        bytes += recomp_utf16_to_utf8(wargv[i], NULL, 0);
    at = ((size_t)argc + 1) * sizeof *argv;
    argv = (char **)malloc(at + bytes + 1);
    if (!argv)
        return NULL;
    text = (char *)argv + at;
    for (i = 0; i < argc; i++) {
        size_t n = recomp_utf16_to_utf8(wargv[i], NULL, 0);
        recomp_utf16_to_utf8(wargv[i], text, n);
        argv[i] = text;
        text += n;
    }
    argv[argc] = NULL;
    return argv;
}

int recomp_win_utf8_args(int *argc, char ***argv)
{
#if defined(_WIN32)
    LPWSTR *wargv;
    char **utf8;
    int n = 0;

    if (GetACP() != CP_UTF8)
        return 0;
    /* A console shows the bytes it is sent in its own code page. */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wargv)
        return 1;                       /* the C runtime's, UTF-8 as well */
    /* wchar_t is 16 bits here. */
    utf8 = recomp_args_from_utf16(n, (const uint16_t *const *)wargv);
    LocalFree(wargv);
    if (utf8 && n > 0) {
        *argc = n;
        *argv = utf8;                   /* kept for the life of the program */
    }
    return 1;
#else
    (void)argc; (void)argv;
    return 1;
#endif
}
