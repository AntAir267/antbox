/*
 * recomp_guest_section.h - the lifted code in a section of its own (Mach-O).
 *
 * Force-include it ahead of each generated source on macOS (clang
 * -include), and every function those sources define, the inline helpers of
 * recomp_types.h included, lands in __TEXT,__guest. The runtime finds the
 * section with getsectiondata() and treats a program counter inside it as
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
#pragma clang section text = "__TEXT,__guest,regular,pure_instructions"
