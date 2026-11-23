# APU state dump (BO-351)

The APU's own view of itself, as a hang-report note.

```
cmake -S tests/apu_dump -B build/apu-dump
cmake --build build/apu-dump
ctest --test-dir build/apu-dump --output-on-failure
```

## Why it exists

The int042 soak froze in the title's "stop a sound and wait until it stopped"
loop, polling a DirectSound voice that never went idle. Only the APU makes a
voice idle, so the hang report now dumps the APU's view: the front end
(`FECTL`), the interrupt line, frames rendered, trap/delivery counters, and
every voice still in a list with its envelope state and buffer position.

`mcpx_apu_dump_state(d, emit)` runs as a crash note (`xbox_crash_add_note` in
`xbox_crash.c`), on the watchdog thread while the rest of the process is stuck,
so it only reads -- no APU lock is taken.

## What it guarantees

The dump formats a fake state: a trapped front end, an up interrupt line, frame
and trap/delivery counters, one active voice in the 2D list, and the APU lock's
state. The test checks the text for those fields, so a change that stops
printing them, or mis-formats the voice's buffer position (`cbo`/`ebo`), fails.

## How it is reachable at all

The dump takes the APU state and an emit callback, so a test passes a capturer
instead of `xbox_crash_printf`. The voice struct is read from guest RAM through
`g_apu_ram_ptr` (the APU's RAM base), pointed at a fake buffer here, the way the
VP itself reads it.
