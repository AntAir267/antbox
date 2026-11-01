"""
Device-register accesses at a constant address, lowered to direct calls.

The runtime reaches the Xbox's trapped device registers -- the MCPX APU and
the USB OHCI controllers -- by faulting: their pages are left inaccessible,
and the SIGSEGV/SIGBUS (or Win32 access-violation) handler decodes the
faulting host instruction and runs the device model (src/platform/
mmio_decode*.h). That is correct for any access, but each one costs a fault
round trip, and DirectSound's PIO traffic is hot: in a Burnout 3 race on the
Steam Deck ~57,000 APU accesses a second (PIO_FREE polls, SET_CURRENT_VOICE
and voice-method writes), which kept the title's main thread busy for
milliseconds a frame (BO-72).

The XDK's DirectSound addresses those registers with absolute addresses
(`mov eax, [0xFE820010]`, `mov [0xFE8202F8], ecx`), so the address is known
when the instruction is lifted. This pass rewrites such a lifted access,

    eax = MEM32(0xFE820010u);         ->  eax = MMIO_RD32(0xFE820010u);
    MEM32(-25033992) = ecx;           ->  MMIO_WR32(-25033992, ecx);

into a call to the runtime's xbox_MmioLoad/xbox_MmioStore
(templates/runtime/recomp_types.h, src/kernel/xbox_memory_layout.c), which
runs the same device handler the trap would, with the same offset and size,
and makes the plain access -- so faults into the trap path as before -- for
anything no model owns at that moment. It is the static half of what an
emulator's memory API does (xemu/QEMU dispatch an MMIO region's accesses to
its MemoryRegionOps by call): the recompiler resolves the region at lift
time when the address is a constant. Accesses through a register keep the
trap; they are the fallback, not something this pass guesses at.

Deliberately narrow, on the emitted C text (after spin-wait lowering, so that
pass still sees the original statements):
  - only MEM8/MEM16/MEM32 with a literal address (hex, or the lifter's signed
    decimal for a negative displacement) inside a window below;
  - a store only as a whole statement `MEMn(lit) = <expr>;`;
  - a line where such an access is the target of anything else (`|=`,
    `++`, ...) is left alone entirely, as is any comment text.
"""

import re

# Guest windows whose registers the runtime traps. The runtime decides what
# inside them is a device at a given moment; this only has to cover them.
#   MCPX APU          0xFE800000-0xFE87FFFF (VP/GP/EP; registers trapped
#                     below 0xFE830000, DSP memories above are RAM)
#   USB OHCI 0 and 1  0xFED00000, 0xFED08000 (4 KB each)
# The AC'97 page (0xFEC00100) is write-trapped with a different mechanism
# (single-step on Win32, mprotect toggling on POSIX) and is cold; not lowered.
MMIO_WINDOWS = (
    (0xFE800000, 0xFE880000),
    (0xFED00000, 0xFED01000),
    (0xFED08000, 0xFED09000),
)

_LIT = r"(0x[0-9A-Fa-f]+u?|-?\d+)"
_ACCESS = re.compile(r"\b(MEM8|MEM16|MEM32)\(" + _LIT + r"\)")
_STORE = re.compile(r"^(\s*)(MEM8|MEM16|MEM32)\(" + _LIT + r"\) = (.+);(\s*)$")
# An access followed by an assignment operator other than the plain store
# handled above, or by ++/--: an lvalue use the pass does not rewrite.
_LVALUE_AFTER = re.compile(r"^\s*(=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--)")
_LVALUE_BEFORE = re.compile(r"(\+\+|--)\s*$|(^|[=(,]\s*)&\s*$")


def literal_value(text):
    """The 32-bit address a literal names (hex with optional u, or signed
    decimal as the lifter prints a negative displacement)."""
    if text.startswith(("0x", "0X")):
        v = int(text.rstrip("uU"), 16)
    else:
        v = int(text)
    return v & 0xFFFFFFFF


def in_window(addr, windows=MMIO_WINDOWS):
    return any(lo <= addr < hi for lo, hi in windows)


def _split_comment(stmt):
    """(code, comment): the statement up to its first comment, and the rest."""
    i = stmt.find("/*")
    if i < 0:
        return stmt, ""
    return stmt[:i], stmt[i:]


def _width(acc):
    return acc[3:]  # "MEM32" -> "32"


def _lower_reads(code, windows):
    def repl(m):
        if not in_window(literal_value(m.group(2)), windows):
            return m.group(0)
        return f"MMIO_RD{_width(m.group(1))}({m.group(2)})"
    return _ACCESS.sub(repl, code)


def lower_stmt(stmt, windows=MMIO_WINDOWS):
    """One emitted statement (or line of statements), with its constant
    device-register accesses lowered. Returns (new_stmt, count)."""
    code, comment = _split_comment(stmt)
    hits = [m for m in _ACCESS.finditer(code)
            if in_window(literal_value(m.group(2)), windows)]
    if not hits:
        return stmt, 0

    store = _STORE.match(code)
    if store and in_window(literal_value(store.group(3)), windows):
        indent, acc, lit, rhs, tail = store.groups()
        # The right-hand side must not itself store to a device literal.
        if any(_LVALUE_AFTER.match(rhs[m.end():])
               for m in _ACCESS.finditer(rhs)):
            return stmt, 0
        rhs_low = _lower_reads(rhs, windows)
        n = 1 + sum(1 for m in _ACCESS.finditer(rhs)
                    if in_window(literal_value(m.group(2)), windows))
        return (f"{indent}MMIO_WR{_width(acc)}({lit}, {rhs_low});{tail}"
                f"{comment}"), n

    # Reads only: every hit must be an rvalue.
    for m in hits:
        if (_LVALUE_AFTER.match(code[m.end():])
                or _LVALUE_BEFORE.search(code[:m.start()])):
            return stmt, 0
    return _lower_reads(code, windows) + comment, len(hits)


def lower_stmts(stmts, windows=MMIO_WINDOWS):
    """A block's statements with lower_stmt applied. Returns (stmts, count)."""
    out, total = [], 0
    for s in stmts:
        new, n = lower_stmt(s, windows)
        out.append(new)
        total += n
    return out, total
