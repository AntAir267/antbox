/*
 * recomp_utf8_args.h - a program's arguments as UTF-8, on Windows too.
 *
 * Text is UTF-8 in this runtime: paths handed to the kernel layer
 * (xbox_path_init), the settings file, the soundtrack library, what a setup
 * window reads from the program's output. On POSIX hosts the arguments and
 * the environment already are. On Windows a program's narrow arguments,
 * environment and file calls are in the "ANSI code page", which is the
 * machine's regional one (1250, 1251, 1252 ...) unless the executable's
 * manifest asks for UTF-8:
 *
 *   <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
 *
 * (Windows 10 1903 and later; older systems ignore it.) With that manifest
 * the C library's fopen, getenv, _putenv, chdir and the Win32 "A" functions
 * all take and give UTF-8, so a folder named in any script opens whether it
 * came from an argument, a variable, %APPDATA% or a file. A port embeds the
 * manifest in its executable and calls recomp_win_utf8_args first thing in
 * main.
 *
 * The title is not affected: what the guest reads and writes as "ANSI" goes
 * through a fixed code page of its own (XBOX_GUEST_CP in kernel.h).
 */
#ifndef RECOMP_UTF8_ARGS_H
#define RECOMP_UTF8_ARGS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-16 (NUL-terminated) to UTF-8. Surrogate pairs become one character, a
 * lone surrogate U+FFFD. Returns the bytes the whole text needs, the NUL
 * included; writes at most `cap` of them, NUL-terminated on a character
 * boundary when cap > 0. */
size_t recomp_utf16_to_utf8(const uint16_t *utf16, char *out, size_t cap);

/* An argument vector from UTF-16 arguments: argc pointers and a NULL, in one
 * block to free(). NULL when out of memory. */
char **recomp_args_from_utf16(int argc, const uint16_t *const *wargv);

/* Windows, first thing in main: when the process's code page is UTF-8 (the
 * manifest above took effect), *argc and *argv are replaced by the wide
 * command line's arguments as UTF-8 (independent of how the C runtime made
 * its own), and a console's code pages are set to UTF-8 so the log reads
 * right. Returns 1 then. Returns 0 and changes nothing when the code page is
 * not UTF-8 (an older Windows, or no manifest): the arguments stay in the
 * ANSI code page, as every narrow call of the program then expects them.
 * Elsewhere: 1, nothing to do. */
int recomp_win_utf8_args(int *argc, char ***argv);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_UTF8_ARGS_H */
