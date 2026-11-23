"""Register locals: guest registers as function locals behave exactly as globals.

The registers are thread-local globals because calls pass them. By default
each function copies them into C locals at entry and writes them
back only where something else can look (translator._localize_registers and
the RECOMP_REGS_OUT/IN macros in recomp_types.h). That is a speed change and
nothing else, so the test is behavioural: small functions that pass arguments
on the stack, return in eax, keep ebx across a call, tail-jump and return
early are translated both ways, compiled against the real runtime header and
run, and every register and the stack must come out the same.

Skips the compiled half if no C compiler is on PATH.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import config
from .translator import FunctionTranslator, _localize_registers

_ROOT = Path(__file__).resolve().parents[2]
BASE = 0x00011000

# caller: push 5; push 7; call ADD2; add esp, 8; mov ebx, eax;
#         call EARLY; add ebx, eax; call TAIL; add eax, ebx; ret
# ADD2:   mov eax, [esp+4]; add eax, [esp+8]; ret
# EARLY:  mov eax, 3; test ebx, ebx; jz out; mov eax, 4; out: ret
#         (ebx is nonzero, so 4, through the early-exit shape anyway)
# TAIL:   mov ecx, 100; jmp LEAF (a tail jump: LEAF returns to the caller)
# LEAF:   lea eax, [ecx+1]; ret
CALLER, ADD2, EARLY, TAIL, LEAF = (BASE, BASE + 0x40, BASE + 0x60,
                                   BASE + 0x80, BASE + 0xA0)


def _rel(frm, size, to):
    return (to - (frm + size)).to_bytes(4, "little", signed=True)


def _image():
    img = bytearray(b"\xCC" * 0xC0)

    def put(at, code):
        img[at - BASE:at - BASE + len(code)] = code

    c = bytearray()
    c += b"\x6A\x05\x6A\x07"                                   # push 5; push 7
    c += b"\xE8" + _rel(CALLER + len(c), 5, ADD2)              # call ADD2
    c += b"\x83\xC4\x08\x8B\xD8"                               # add esp,8; mov ebx,eax
    c += b"\xE8" + _rel(CALLER + len(c), 5, EARLY)             # call EARLY
    c += b"\x03\xD8"                                           # add ebx, eax
    c += b"\xE8" + _rel(CALLER + len(c), 5, TAIL)              # call TAIL
    c += b"\x03\xC3\xC3"                                       # add eax, ebx; ret
    put(CALLER, c)
    put(ADD2, b"\x8B\x44\x24\x04\x03\x44\x24\x08\xC3")
    put(EARLY, b"\xB8\x03\x00\x00\x00\x85\xDB\x74\x05\xB8\x04\x00\x00\x00\xC3")
    t = bytearray(b"\xB9\x64\x00\x00\x00")                     # mov ecx, 100
    t += b"\xE9" + _rel(TAIL + len(t), 5, LEAF)                # jmp LEAF
    put(TAIL, t)
    put(LEAF, b"\x8D\x41\x01\xC3")                             # lea eax,[ecx+1]; ret
    return bytes(img)


_FUNCS = {CALLER: 0x1D, ADD2: 9, EARLY: 15, TAIL: 10, LEAF: 4}

# Every global config._install writes, so one test cannot leak into another.
_CONFIG_GLOBALS = (
    "_SECTIONS", "SECTIONS", "_configured_from",
    "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
    "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
)

_HARNESS = r"""
@DEFINES@
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_ebp, g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static int s_mismatches;
void recomp_regs_mismatch(const char *reg, const char *func, int line,
                          uint32_t local, uint32_t global)
{ (void)reg; (void)func; (void)line; (void)local; (void)global; s_mismatches++; }
@DECLS@
@BODIES@
int main(void) {
    static uint8_t guest[0x10000];
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    g_esp = 0x8000u; g_ebx = 0x1234u; g_esi = 0x55u; g_edi = 0x66u;
    g_esp -= 4; *(uint32_t *)(guest + g_esp) = 0xDEADu;   /* return address */
    sub_00011000();
    printf("eax=%u ebx=%u ecx=%u esi=%X edi=%X esp=%X\n",
           g_eax, g_ebx, g_ecx, g_esi, g_edi, g_esp);
    if (s_mismatches) { printf("mismatches %d\n", s_mismatches); return 1; }
    return 0;
}
"""


class RegLocalsTest(unittest.TestCase):
    def setUp(self):
        self._saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS}
        image = _image()
        config._install(
            [config.Section(".text", BASE, len(image), 0, len(image), True)],
            entry_point=BASE, kernel_thunk_addr=BASE, origin="reg-locals-test")
        self.image = image

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(config, k, v)

    def _translate(self, reg_locals, verify=False):
        db = {a: {"start": f"0x{a:08X}", "end": a + n, "_addr": a, "size": n}
              for a, n in _FUNCS.items()}
        t = FunctionTranslator(self.image, db)
        t.reg_locals = reg_locals
        t.reg_verify = verify
        return {a: t.translate_function(a, db[a]) for a in db}

    # -- the text --

    def test_entry_copies_in_and_every_exit_writes_back(self):
        c = self._translate(True)[EARLY]
        body = c[c.index("{"):]
        self.assertTrue(body.split("\n")[1].lstrip().startswith(
            "uint32_t eax = g_eax"), c)
        # Every return writes back what the function writes (eax, esp here)
        # and nothing it doesn't; the bottom too.
        for ret in re.findall(r"\{ ([^{}]*)return; \}", c):
            self.assertEqual("g_eax = eax; g_esp = esp; ", ret, c)
        self.assertNotIn("g_ebx = ebx", c)
        self.assertIn("RECOMP_REGS_OUT() RECOMP_REGS_OUT_ALL()\n", c)

    def test_default_output_has_no_sync(self):
        for c in self._translate(False).values():
            self.assertNotIn("RECOMP_REGS", c)
            self.assertNotIn("uint32_t eax", c)

    def test_bare_calls_tails_and_landings(self):
        lines = [
            "    PUSH32(esp, 0x10u); sub_00011040(); /* call */",
            "    g_seh_ebp = ebp; sub_00011040(); return; /* tail */",
            "    fp_pop(); return;",
            "    if (_flags) return;",
            "    { int _sjv = setjmp(*recomp_setjmp_slot(MEM32(esp))); if (_sjv) { ebp = g_seh_ebp; eax = (uint32_t)_sjv; } }",
            "    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)",
        ]
        out = _localize_registers(lines)
        # The pushes write esp; the setjmp landing writes eax. Each write-back
        # carries only the registers written since the last sync.
        self.assertIn("{ g_esp = esp; sub_00011040(); RECOMP_REGS_IN(); }", out[0])
        # A bare call (a tail jump) doesn't count as a sync for what follows:
        # the conservative side, so esp is written back again.
        self.assertIn("{ g_esp = esp; sub_00011040(); return; }", out[1])
        self.assertEqual("    fp_pop(); { g_esp = esp; return; }", out[2])
        self.assertEqual("    if (_flags) { g_esp = esp; return; }", out[3])
        self.assertIn("if (_sjv) { RECOMP_REGS_IN(); ebp = g_seh_ebp;", out[4])
        self.assertEqual(lines[5], out[5])

    def test_a_call_used_as_a_value_is_refused(self):
        with self.assertRaises(ValueError):
            _localize_registers(["    eax = some_helper();"])

    # -- the behaviour --

    def _run(self, reg_locals, verify=False):
        cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        bodies = self._translate(reg_locals, verify)
        src = (_HARNESS
               .replace("@DEFINES@", "#define RECOMP_GENERATED_CODE\n"
                        + ("#define RECOMP_REG_LOCALS\n" if reg_locals else ""))
               .replace("@DECLS@", "\n".join(f"void sub_{a:08X}(void);" for a in bodies))
               .replace("@BODIES@", "\n".join(bodies.values())))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rl.c"
            path.write_text(src)
            exe = Path(tmp) / "rl.exe"
            cmd = [cc, "-w", "-O2", str(path), "-o", str(exe),
                   "-I", str(_ROOT / "templates" / "runtime"), "-I", str(_ROOT / "src")]
            if os.name != "nt":
                cmd.append("-lm")
            built = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-3000:] + "\n" + src)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            return ran.stdout.strip()

    def test_locals_and_globals_agree(self):
        globals_out = self._run(False)
        # 5 + 7 = 12 into ebx, + 4 from EARLY = 16, + 101 from the tail = 117;
        # the caller's own ret pops the return address the harness pushed.
        self.assertEqual(
            "eax=117 ebx=16 ecx=100 esi=55 edi=66 esp=8000", globals_out)
        self.assertEqual(globals_out, self._run(True))

    def test_verify_finds_no_skipped_register_out_of_step(self):
        """--reg-verify checks every register a narrowed write-back skips."""
        c = "\n".join(self._translate(True, verify=True).values())
        self.assertIn("RECOMP_REGS_SKIPPED(", c)
        self.assertEqual("eax=117 ebx=16 ecx=100 esi=55 edi=66 esp=8000",
                         self._run(True, verify=True))


if __name__ == "__main__":
    unittest.main()
