# Kernel call count (BO-351)

The log gate that throttles the per-call kernel trace. No game files and no
mapped guest window are needed.

```
cmake -S tests/kernel_call_count -B build/kernel-call-count
cmake --build build/kernel-call-count
ctest --test-dir build/kernel-call-count --output-on-failure
```

## Why it exists

`KERNEL_LOG_ON()` in `src/kernel/kernel_bridge.c` logs each kernel call until
the call counter passes a budget, then goes quiet so a busy title does not
flood the console. The counter was a signed 32-bit `int`. Burnout 3's title
spins in a "stop a sound and wait until it stopped" loop that makes kernel
calls as fast as it can; on the int042 soak it wrapped the counter past 2^31,
the count went negative, `count <= budget` became true again, and full tracing
switched back on -- 9.5 M lines of trace, which hid every later freeze (BO-351).

The counter is now `unsigned long long`, so it cannot wrap within any run.

## What it guarantees

**The trace starts on.** At count 0 the gate is open.

**The trace stays off past 2^31.** At `2^31`, `2^31 + 1`, and `2^64 - 1` the
gate is shut. With the old signed type the first of these re-opened it, so this
check fails without the fix.

## How the gate is reachable at all

The counter and the gate live behind `static` in `kernel_bridge.c`. For this
test they are exposed as `xbox_kernel_log_on()`, `xbox_kernel_log_on_half()`
and `xbox_kernel_set_call_count()` -- the same predicate the `KERNEL_LOG_ON()`
macro now calls, so asserting the gate is shut *is* asserting tracing stays
off. The setter jumps the counter past its old wrap point rather than making
two billion real calls.

The `recomp_lookup` / `recomp_lookup_manual` stubs at the top of `test_main.c`
are the same stand-ins `tests/kernel_bridge` needs: pulling `kernel_bridge.o`
in also pulls its references to the generated title, which a regression harness
does not have. Nothing here calls a guest function.
