/* utf8_args: UTF-16 arguments to UTF-8 (recomp_utf8_args.h). */
#include "recomp_utf8_args.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

int main(int argc, char **argv)
{
    /* "Zenék őű музыка" */
    static const uint16_t folder[] = { 'Z', 'e', 'n', 0xE9, 'k', ' ', 0x151, 0x171, ' ',
                                       0x43C, 0x443, 0x437, 0x44B, 0x43A, 0x430, 0 };
    static const char folder_utf8[] = "Zen\xC3\xA9k \xC5\x91\xC5\xB1 "
                                      "\xD0\xBC\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0";
    /* C:\Users\Zoé\AppData, a CJK name, U+1F3B5 as a surrogate pair. */
    static const uint16_t path[] = { 'C', ':', '\\', 'U', 's', 'e', 'r', 's', '\\', 'Z', 'o',
                                     0xE9, '\\', 0x65E5, 0x672C, ' ', 0xD83C, 0xDFB5, 0 };
    static const char path_utf8[] = "C:\\Users\\Zo\xC3\xA9\\\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB5";
    static const uint16_t lone[] = { 'a', 0xD83C, 'b', 0xDFB5, 'c', 0 };
    static const uint16_t empty[] = { 0 };
    static const uint16_t plain[] = { '-', '-', 'm', 'u', 's', 'i', 'c', '-', 'd', 'i', 'r', 0 };
    const uint16_t *wargv[5];
    char buf[128], **out;
    size_t n;

    n = recomp_utf16_to_utf8(folder, buf, sizeof buf);
    CHECK("Latin Extended and Cyrillic", !strcmp(buf, folder_utf8) && n == sizeof folder_utf8);
    n = recomp_utf16_to_utf8(path, buf, sizeof buf);
    CHECK("a path, CJK, a surrogate pair", !strcmp(buf, path_utf8) && n == sizeof path_utf8);
    recomp_utf16_to_utf8(lone, buf, sizeof buf);
    CHECK("lone surrogates", !strcmp(buf, "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" "c"));
    n = recomp_utf16_to_utf8(empty, buf, sizeof buf);
    CHECK("empty", buf[0] == 0 && n == 1);
    CHECK("size only", recomp_utf16_to_utf8(folder, NULL, 0) == sizeof folder_utf8);
    /* A short buffer: cut on a character, the full size still returned. */
    memset(buf, 'x', sizeof buf);
    n = recomp_utf16_to_utf8(folder, buf, 5);        /* "Zen" + the 2-byte e-acute does not fit */
    CHECK("cut on a character", !strcmp(buf, "Zen") && n == sizeof folder_utf8);

    wargv[0] = plain; wargv[1] = folder; wargv[2] = empty; wargv[3] = path; wargv[4] = NULL;
    out = recomp_args_from_utf16(4, wargv);
    CHECK("an argument vector", out && !strcmp(out[0], "--music-dir") && !strcmp(out[1], folder_utf8)
                                && out[2][0] == 0 && !strcmp(out[3], path_utf8) && out[4] == NULL);
    free(out);
    out = recomp_args_from_utf16(0, wargv);
    CHECK("no arguments", out && out[0] == NULL);
    free(out);

    /* The program's own arguments. Off Windows: untouched. On Windows: from
     * the wide command line when the code page is UTF-8. */
    {
        int ac = argc;
        char **av = argv;
        int utf8 = recomp_win_utf8_args(&ac, &av);
#if defined(_WIN32)
        printf("code page %u: arguments %s\n", GetACP(), utf8 ? "taken as UTF-8" : "left as they are");
        CHECK("says UTF-8 only when it is", utf8 == (GetACP() == CP_UTF8));
        if (ac >= 3 && !strcmp(av[1], "--echo")) {
            printf("got: %s\n", av[2]);
            if (utf8)
                CHECK("the command line's letters", !strcmp(av[2], folder_utf8));
        }
#else
        CHECK("nothing to do off Windows", utf8 == 1 && ac == argc && av == argv);
#endif
    }

    printf(failures ? "%d FAILED\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
