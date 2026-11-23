/*
 * recomp_guest_section.h - the lifted code in a section of its own.
 *
 * Force-include it ahead of each generated source (clang -include), and
 * every function those sources define, the inline helpers of
 * recomp_types.h included, lands in one section: __TEXT,__guest on macOS
 * (found with getsectiondata()), guest_text on ELF hosts -- Linux, the
 * Deck, Android -- (found through the linker's __start_guest_text and
 * __stop_guest_text). The runtime treats a program counter inside it as
 * guest code: under RECOMP_DEBUG=guest_cpus=token a thread there can be
 * preempted at any instruction for another guest thread, while one in host
 * code (which may hold a C library or device lock) hands over only at its
 * next kernel call's return (src/kernel/kernel_thread.c,
 * src/platform/win32_compat.c).
 *
 * From vyanhursky/DefJamFFNY-recomp src/recomp/guest_section.h, which the
 * Def Jam port force-includes for the same token (vyanhursky/xboxrecomp
 * dc00f70). Without the token the section only moves code; nothing reads it.
 */
#if defined(__APPLE__)
#pragma clang section text = "__TEXT,__guest,regular,pure_instructions"
#elif defined(__clang__) && defined(__ELF__)
#pragma clang section text = "guest_text"
#endif
/* Windows has no token (guest_cpus=token pins the guest threads to one core
 * there, docs/runtime/guest-cpus.md), and GCC has no section pragma: this
 * header does nothing for them. */
