"""The atomic lifts, compiled against the real runtime header and run.

test_lifter_atomics.py checks the C the lifter writes; this compiles that C
with templates/runtime/recomp_types.h (so the RECOMP_ATOMIC_* macros are the
ones the title gets) and runs each fixture on this host: the memory word
after the instruction, the register it hands back, and the branch after it.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest

from . import config
from .translator import FunctionTranslator

_RUNTIME = Path(__file__).resolve().parents[2] / "templates" / "runtime"

# jcc +3 over `xor eax, eax; ret`, landing on `mov eax, 1; ret`: the
# function returns 1 when the branch is taken.
_TAIL = "31c0c3b801000000c3"
_FIXTURES = {
    "lock_add_jz":      "f00101" + "7403" + _TAIL,
    "lock_xadd_jb":     "f00fc101" + "7203" + _TAIL,
    "cmpxchg_jb":       "0fb111" + "7203" + _TAIL,
    "cmpxchg_ret":      "0fb111c3",
    "cmpxchg8b_jz":     "0fc709" + "7403" + _TAIL,
    "lock_bts_jb":      "f00fab01" + "7203" + _TAIL,
    "lock_dec_jz":      "f0ff09" + "7403" + _TAIL,
    "lock_neg_ret":     "f0f719c3",
    "xadd16_ret":       "660fc101c3",
    "cmpxchg8_jz":      "0fb011" + "7403" + _TAIL,
    "lock_and_imm_jz":  "f0812100ff0000" + "7403" + _TAIL,
}

_MAIN = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint32_t eax, ebx, ecx, edx, esp;
ptrdiff_t g_xbox_mem_offset;
static uint8_t guest[0x10000];
#define GUEST 0x20000u
static int fails;
static void check(const char *what, uint64_t got, uint64_t want)
{
    if (got != want) { fprintf(stderr, "%s: got %llx want %llx\n", what,
                               (unsigned long long)got, (unsigned long long)want); fails++; }
}
static void reset(uint32_t word)
{
    memset(guest, 0, sizeof guest);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest - (ptrdiff_t)GUEST;
    ecx = GUEST; esp = 0x1000; eax = ebx = edx = 0;
    MEM32(GUEST) = word;
}
int main(void)
{
    reset(0xFFFFFFF0u); eax = 0x10; lock_add_jz();
    check("lock add: sum", MEM32(GUEST), 0); check("lock add: ZF", eax, 1);
    reset(5); eax = 3; lock_add_jz();
    check("lock add: sum 2", MEM32(GUEST), 8); check("lock add: ZF 2", eax, 0);

    reset(0xFFFFFFFFu); eax = 1; lock_xadd_jb();
    check("lock xadd: sum", MEM32(GUEST), 0); check("lock xadd: CF", eax, 1);
    reset(7); eax = 1; lock_xadd_jb();
    check("lock xadd: sum 2", MEM32(GUEST), 8); check("lock xadd: CF 2", eax, 0);

    reset(10); eax = 10; edx = 99; cmpxchg_jb();
    check("cmpxchg: swapped", MEM32(GUEST), 99); check("cmpxchg: not below", eax, 0);
    reset(10); eax = 3; edx = 99; cmpxchg_jb();
    check("cmpxchg: kept", MEM32(GUEST), 10); check("cmpxchg: eax < dest", eax, 1);
    reset(10); eax = 3; edx = 99; cmpxchg_ret();
    check("cmpxchg: eax loaded", eax, 10);
    reset(10); eax = 10; edx = 99; cmpxchg_ret();
    check("cmpxchg: eax kept on match", eax, 10);

    reset(0); MEM32(GUEST) = 0x11111111u; MEM32(GUEST + 4) = 0x22222222u;
    eax = 0x11111111u; edx = 0x22222222u; ebx = 0xAAAAAAAAu;
    /* ecx is both the address and the new high word, so the high word
       stored on a match is GUEST itself. */
    cmpxchg8b_jz();
    check("cmpxchg8b: lo", MEM32(GUEST), 0xAAAAAAAAu);
    check("cmpxchg8b: hi", MEM32(GUEST + 4), GUEST);
    check("cmpxchg8b: ZF", eax, 1);
    reset(0); MEM32(GUEST) = 0x11111111u; MEM32(GUEST + 4) = 0x22222222u;
    eax = 1; edx = 2; ebx = 0xAAAAAAAAu; cmpxchg8b_jz();
    check("cmpxchg8b: lo kept", MEM32(GUEST), 0x11111111u);
    check("cmpxchg8b: edx loaded", edx, 0x22222222u);
    check("cmpxchg8b: ZF clear", eax, 0);

    reset(0x4); eax = 2; lock_bts_jb();
    check("lock bts: bit was set", eax, 1); check("lock bts: word", MEM32(GUEST), 4);
    reset(0x4); eax = 40; lock_bts_jb();    /* bit 40: dword 1, bit 8 */
    check("lock bts: bit 40 was clear", eax, 0);
    check("lock bts: dword 1", MEM32(GUEST + 4), 0x100);

    reset(1); lock_dec_jz(); check("lock dec: zero", MEM32(GUEST), 0); check("lock dec: ZF", eax, 1);
    reset(0); lock_dec_jz(); check("lock dec: wrap", MEM32(GUEST), 0xFFFFFFFFu); check("lock dec: ZF 2", eax, 0);

    reset(5); lock_neg_ret(); check("lock neg", MEM32(GUEST), 0xFFFFFFFBu);

    reset(0x12345678u); eax = 0xFFFF0001u; xadd16_ret();
    check("xadd16: word", MEM32(GUEST), 0x12345679u);
    check("xadd16: ax = old, eax high kept", eax, 0xFFFF5678u);

    reset(0x000000AAu); eax = 0xFFFFFFAAu; edx = 0x55; cmpxchg8_jz();
    check("cmpxchg8: byte swapped", MEM32(GUEST), 0x55); check("cmpxchg8: ZF", eax, 1);
    reset(0x000000ABu); eax = 0xFFFFFFAAu; edx = 0x55; cmpxchg8_jz();
    check("cmpxchg8: byte kept", MEM32(GUEST), 0xAB); check("cmpxchg8: ZF clear", eax, 0);

    reset(0x12345678u); lock_and_imm_jz();
    check("lock and imm: word", MEM32(GUEST), 0x5600u); check("lock and imm: ZF clear", eax, 0);
    reset(0x120000FFu); lock_and_imm_jz();
    check("lock and imm: zero", MEM32(GUEST), 0); check("lock and imm: ZF", eax, 1);
    return fails ? 1 : 0;
}
'''


def test_atomic_lifts_compiled_and_run():
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\Program Files\LLVM\bin\clang.exe").exists():
        cc = r"C:\Program Files\LLVM\bin\clang.exe"
    if not cc:
        pytest.skip("C compiler unavailable")
    code = '#include "recomp_types.h"\nextern uint32_t eax, ebx, ecx, edx, esp;\n'
    for name, hexs in _FIXTURES.items():
        image = bytes.fromhex(hexs)
        base = 0x10000
        config._install([config.Section(".text", base, len(image), 0,
                                        len(image), True)],
                        entry_point=base, kernel_thunk_addr=base,
                        origin="atomics-compiled")
        db = {base: {"start": hex(base), "end": base + len(image),
                     "_addr": base, "size": len(image)}}
        c = FunctionTranslator(image, db).translate_function(base, db[base])
        assert "RECOMP_UNIMPL" not in c, (name, c)
        code += c.replace("sub_00010000", name)
    code += _MAIN
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / "test.c"
        source.write_text(code)
        exe = Path(temp) / ("test.exe" if os.name == "nt" else "test")
        result = subprocess.run([cc, "-O1", "-I", str(_RUNTIME), str(source),
                                 "-o", str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
