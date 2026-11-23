# Guest CPUs: `RECOMP_DEBUG=guest_cpus=...`

The Xbox has one CPU. Two threads of a title never run at the same instant,
and a lower-priority thread does not run while a higher-priority one is
ready. Titles lean on that without saying so. Here the title's threads are
host threads, and this switch decides how many host CPUs they may use at
once.

The threads it covers are the ones the title schedules: the main guest
thread and every `PsCreateSystemThreadEx` worker. The kernel timer thread
(DPCs, timer routines, the vblank) and the device threads (APU, OHCI,
NV2A) are left out on purpose. They stand in for interrupts and are
ordered against guest code by the one-CPU gate (`kernel_hal.c`), not by
this switch. Details and measurements are in the comment above
`xbox_GuestCpuMask` in `src/kernel/kernel_thread.c`.

## Values

| value | what happens |
|---|---|
| unset | the toolkit default: every guest thread pinned to one host core. A title can opt out with `xbox_GuestCpusDefaultUnpinned()` (Burnout 3 does on Windows); then unset means unpinned. |
| `all` | unpinned: guest threads run on every core, side by side. |
| `<n>` | pinned to core `n`, if the process may use it. |
| `token` | one guest thread runs at a time, on any core (see below). |

`guest_cpus` overrides a title's `xbox_GuestCpusDefaultUnpinned()`.
`all` and `token` are the two sides of an A/B: if a crash shows up
unpinned and never with `token`, it is in the "priority or one CPU was the
lock" class, and the fix is a lock around the structure.

## `token` on each platform

| host | how `token` holds the guest threads to one CPU |
|---|---|
| macOS | a token (`win32_compat.c`, from vyanhursky/xboxrecomp dc00f70). Every blocking primitive gives it up; a thread that waited 1 ms preempts the holder at once in the lifted code (`__TEXT,__guest`), at its next kernel call in host code. |
| Linux, Steam Deck, Android | the same token. Preemption in the lifted code needs the `guest_text` section: build the generated sources with clang and `-include templates/runtime/recomp_guest_section.h`. Without it a guest thread changes over only at kernel calls and waits, and one that spins with no kernel call keeps the token. |
| Windows | no token: the system's own blocking calls cannot give it up. `token` pins the guest threads to one core (the first even core from 2 on), the same as an unset value without the title's opt-out. The log says so once: `guest_cpus=token on Windows: the guest threads are pinned to one core`. |

The startup log names the mode:

- `guest threads take turns on one guest CPU (guest_cpus=token); preempted in the lifted code`: token with the section.
- `... no guest code section (recomp_guest_section.h), so they change over only at kernel calls and waits`: token without it.
- `guest threads on one host core (cpu N)`: pinned.
- nothing about guest threads: unpinned.

## Setting it

```
RECOMP_DEBUG=guest_cpus=token ./Takedawn ...
RECOMP_DEBUG=guest_cpus=all   ./Takedawn ...
```

`RECOMP_GUEST_CPUS=<value>` is the older spelling of the same switch.

Several `RECOMP_DEBUG` switches go in one variable, comma-separated
(`src/platform/recomp_env.h`; `RECOMP_DEBUG=help` prints the table).
Tests: `tests/guest_turn_posix` (the token), `tests/guest_section` (the
section is found).
