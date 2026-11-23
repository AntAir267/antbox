/*
 * RECOMP_DEBUG=guest_cpus=token finds the title's lifted code (audit
 * 2026-11-03, PLAN item 5: the token has to work on every platform the
 * soak A/Bs on).
 *
 * guest_fn.c is built the way a title's generated sources are, with
 * templates/runtime/recomp_guest_section.h force-included. A guest thread
 * that joins the token must then know where the lifted code is, or it can
 * only be preempted at kernel calls and a spinning guest thread keeps the
 * CPU. macOS has had that (__TEXT,__guest); ELF hosts (Linux, the Deck,
 * Android) had no section and no lookup.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(unsigned int va);
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return NULL; }

int guest_fn(int x);
void xbox_GuestThreadPin(const char *role);

#if defined(__ELF__) && !defined(__APPLE__)
extern const char __start_guest_text[] __attribute__((weak, visibility("hidden")));
extern const char __stop_guest_text[] __attribute__((weak, visibility("hidden")));
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    recomp_env_set(RENV_GUEST_CPUS, "token");
    xbox_GuestThreadPin("guest-main");
    check(guest_turn_joined(), "the guest thread joined the token");
    check(guest_turn_has_code(), "the token knows where the lifted code is");
#if defined(__ELF__) && !defined(__APPLE__)
    check(__start_guest_text && (const char *)(uintptr_t)guest_fn >= __start_guest_text
          && (const char *)(uintptr_t)guest_fn < __stop_guest_text,
          "the lifted function is inside guest_text");
#endif
    check(guest_fn(2) == 7, "the lifted function runs");
    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
