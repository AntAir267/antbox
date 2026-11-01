/* kernel_fd_owner: a file handle whose descriptor some other code closed
 * is caught (BO-221). The handle keeps reading through the number, and once
 * another file has it the reads are that file's: the check says so at the
 * read, and the open that took the number says whose it still was. */
#include "win32_compat.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static void make(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    char a[64], b[64];
    HANDLE ha, hb;
    int fa, fb;

    snprintf(a, sizeof a, "/tmp/fd_owner_a_%d", (int)getpid());
    snprintf(b, sizeof b, "/tmp/fd_owner_b_%d", (int)getpid());
    make(a, "the bank");
    make(b, "the song");

    fa = open(a, O_RDONLY);
    ha = w32_open_handle(fa, a);
    CHECK("a fresh handle names its file", w32_handle_check(ha, "test") == 1);

    close(fa);                            /* behind the handle's back */
    fb = open(b, O_RDONLY);               /* the lowest number: fa's */
    CHECK("the number went to the next open", fb == fa);
    hb = w32_open_handle(fb, b);          /* logs REUSED */
    CHECK("the first handle's reads are another file's now", w32_handle_check(ha, "test") == 0);
    CHECK("the second handle names its file", w32_handle_check(hb, "test") == 1);

    CloseHandle(hb);
    unlink(a);
    unlink(b);
    printf(failures ? "%d FAILED\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
