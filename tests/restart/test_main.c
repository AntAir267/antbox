/* restart: recomp_restart.h. */
#include "recomp_restart.h"
#include "recomp_instance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#define getpid _getpid
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static void later_handler(void)
{
    /* Registered after recomp_restart_init, so it runs before the restart:
     * a program's own shutdown work is done by then. */
    FILE *f = fopen("restart_order.txt", "a");
    if (f) { fputs("shutdown\n", f); fclose(f); }
}

int main(int argc, char **argv)
{
    recomp_restart_init(argc, argv);
    atexit(later_handler);

    if (argc >= 2 && !strcmp(argv[1], "snapshot")) {
        int ac = 0;
        char **av = NULL, **env = NULL;
        const char *cwd = NULL;
        int i, found = 0;

        CHECK("available", recomp_restart_available());
        CHECK("not asked for", !recomp_restart_requested());
        /* What the program does to itself after the start is not in it. */
        set_env("RESTART_TEST_LATER", "1");
        argv[1] = "changed";
        recomp_restart_snapshot(&ac, &av, &env, &cwd);
        CHECK("argument count", ac == argc);
        CHECK("the arguments as given", av && !strcmp(av[1], "snapshot") && !strcmp(av[2], "a b")
                                        && av[ac] == NULL);
        CHECK("a directory", cwd && *cwd);
#if !defined(_WIN32)
        for (i = 0; env && env[i]; i++)
            if (!strncmp(env[i], "RESTART_TEST_LATER=", 19))
                found = 1;
        CHECK("the environment as given", env && !found);
#else
        (void)i; (void)found;
#endif
        recomp_restart_request("a test");
        CHECK("asked for", recomp_restart_requested());
        recomp_restart_cancel();
        CHECK("taken back", !recomp_restart_requested());
        printf(failures ? "%d FAILED\n" : "ok\n", failures);
        return failures ? 1 : 0;       /* exits without restarting */
    }

    /* An AppImage (recomp_restart.c, plan_appimage): what the exec would
     * start, from a made environment of the runtime's (CMakeLists.txt). */
    if (argc >= 2 && !strcmp(argv[1], "appimage")) {
        const char *exe;
        char **av, **ev;
        int i, has_dir = 0, has_img = 0;
        const char *ld = NULL, *path = NULL, *xdg = NULL;
        /* The runtime's variables come from the test's environment (as
         * the runtime sets them before the program starts); APPIMAGE is
         * this test's own file. */
        recomp_restart_target(&exe, &av, &ev);
        CHECK("the image file is started again", exe && getenv("APPIMAGE") && !strcmp(exe, getenv("APPIMAGE")));
        CHECK("named as its runtime was", av && !strcmp(av[0], "./Takedawn.AppImage"));
        CHECK("the arguments come along", av[1] && !strcmp(av[1], "appimage") && av[2] && !strcmp(av[2], "x y"));
        for (i = 0; ev && ev[i]; i++) {
            has_dir |= !strncmp(ev[i], "APPDIR=", 7) || !strncmp(ev[i], "ARGV0=", 6) || !strncmp(ev[i], "OWD=", 4);
            has_img |= !strncmp(ev[i], "APPIMAGE=", 9);
            if (!strncmp(ev[i], "LD_LIBRARY_PATH=", 16)) ld = ev[i] + 16;
            if (!strncmp(ev[i], "PATH=", 5)) path = ev[i] + 5;
            if (!strncmp(ev[i], "XDG_DATA_DIRS=", 14)) xdg = ev[i];
        }
        CHECK("without the runtime's variables", !has_dir && !has_img);
        CHECK("the old mount out of the library path", ld && !strcmp(ld, "/opt/lib"));
        CHECK("and out of PATH", path && !strcmp(path, "/usr/bin:/bin"));
        CHECK("a list of only the old mount goes", xdg == NULL);
        printf(failures ? "%d FAILED\n" : "ok (appimage)\n", failures);
        return failures ? 1 : 0;
    }

    /* The same, done: the first life restarts, the second is the image
     * file started again (this test) with the runtime's variables gone. */
    if (argc == 3 && !strcmp(argv[1], "appimage-real")) {
        FILE *f = fopen(argv[2], "r");
        const char *ld = getenv("LD_LIBRARY_PATH");
        if (!f) {
            f = fopen(argv[2], "w");
            if (!f)
                return 2;
            fclose(f);
            recomp_restart_request("the AppImage test's first life");
            return 0;
        }
        fclose(f);
        remove(argv[2]);
        CHECK("second life: named as the runtime was", !strcmp(argv[0], "./Takedawn.AppImage"));
        CHECK("second life: no old mount", !getenv("APPDIR") && !getenv("APPIMAGE") && !getenv("ARGV0"));
        CHECK("second life: library path without it", ld && !strcmp(ld, "/opt/lib"));
        printf(failures ? "%d FAILED\n" : "ok (appimage, restarted once)\n", failures);
        return failures ? 1 : 0;
    }

    if (argc == 3 && !strcmp(argv[1], "probe"))
        /* Another copy: 0 when it finds the lock held, as it should. */
        return recomp_instance_lock(argv[2], 0) == 0 ? 0 : 1;

    if (argc >= 4 && !strcmp(argv[1], "real")) {
        const char *mark = argv[2];
        FILE *f = fopen(mark, "r");
        if (!f) {
            char cmd[1200];
            /* First life: leave the mark, dirty the environment, restart. */
            f = fopen(mark, "w");
            if (!f)
                return 2;
            /* One copy per save root (recomp_instance.h): this one holds
             * it, another is refused, and the restart gets it back. */
            if (recomp_instance_lock(".", 0) != 1)
                return 3;
            snprintf(cmd, sizeof cmd, "\"%s\" probe .", argv[0]);
            if (system(cmd) != 0)
                return 4;
            fprintf(f, "%ld\n", (long)getpid());
            fclose(f);
            remove("restart_order.txt");
            set_env("RESTART_TEST_GIVEN", "changed by the first life");
            set_env("RESTART_TEST_LATER", "1");
            recomp_restart_request("the test's first life");
            return 0;                   /* the ordinary exit: the restart follows */
        } else {
            long first = 0;
            const char *given = getenv("RESTART_TEST_GIVEN");
            FILE *o;
            char line[32] = "";
            if (fscanf(f, "%ld", &first) != 1)
                first = 0;
            fclose(f);
            remove(mark);
            CHECK("the argument came along", !strcmp(argv[3], "an argument"));
            CHECK("the environment as it was given", given && !strcmp(given, "given"));
            CHECK("not what the first life added", getenv("RESTART_TEST_LATER") == NULL);
#if !defined(_WIN32)
            CHECK("the same process", first == (long)getpid());
#else
            CHECK("another process on Windows", first != 0 && first != (long)getpid());
#endif
            o = fopen("restart_order.txt", "r");
            CHECK("the first life's shutdown ran before the restart",
                  o && fgets(line, sizeof line, o) && !strncmp(line, "shutdown", 8));
            if (o) fclose(o);
            CHECK("no second restart", !recomp_restart_requested());
            CHECK("the restart got the lock back", recomp_instance_lock(".", 5000) == 1);
            printf(failures ? "%d FAILED\n" : "ok (restarted once)\n", failures);
            return failures ? 1 : 0;
        }
    }
    printf("usage: restart_test snapshot ... | real <mark file> <argument>\n");
    return 2;
}
