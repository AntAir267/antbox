"""x87 instructions that used to vanish: lifted, and run.

Until 2026-11-23 these fell to the float lifter's last line, a bare
`/* FPU: ... */` comment, so the instruction did nothing and nothing said so.
Each game had some in its generated C: DS2 four ficomp (no compare and no pop,
so every st(i) after it is one slot off), MA2 five fcmovnbe and an fxtract, all
three an fnsave/frstor pair. And every other exit the lifter has for a form it
cannot translate now goes through _unimplemented (RECOMP_UNIMPL), so nothing
is dropped silently any more.

The emitted C is compiled against the real runtime header and run.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import FP_STACK_MACROS

_ROOT = Path(__file__).resolve().parents[2]


def _insn(address, size, mnemonic, op_str="", operands=()):
    i = Instruction(address, size, mnemonic, op_str, "")
    i.operands = list(operands)
    return i


def _mem(disp, size):
    return Operand(type="mem", mem_disp=disp, mem_size=size)


def _st(n):
    return Operand(type="reg", reg=f"st({n})")


def _block(instructions):
    stmts = lift_basic_block(Lifter(), BasicBlock(start=0, instructions=instructions))[0]
    return "\n".join("    " + s for s in stmts)


def _lift(insn):
    return "\n".join(Lifter().lift_instruction(insn))


class EmitTest(unittest.TestCase):
    def test_no_bare_comment_for_what_used_to_vanish(self):
        for m, ops in (("ficomp", [_mem(0x100, 2)]), ("ficom", [_mem(0x100, 4)]),
                       ("fxtract", []), ("fnsave", [_mem(0x200, 108)]),
                       ("frstor", [_mem(0x200, 108)]), ("fninit", []), ("fnclex", [])):
            with self.subTest(m=m):
                out = _lift(_insn(0, 2, m, "x", ops))
                self.assertNotIn("/* FPU:", out)
                self.assertNotIn("RECOMP_UNIMPL", out)

    def test_unknown_forms_are_marked_not_dropped(self):
        lifter = Lifter()
        out = "\n".join(lifter.lift_instruction(_insn(0x1234, 2, "fisttp", "word ptr [eax]",
                                                       [_mem(0, 2)])))
        self.assertIn('RECOMP_UNIMPL("fisttp', out)
        out = "\n".join(lifter.lift_instruction(_insn(0x1236, 2, "fbstp", "x", [_mem(0, 10)])))
        self.assertIn('RECOMP_UNIMPL("fbstp', out)
        self.assertIn(0x1236, lifter.unimplemented["fbstp"])

    def test_fcmov_without_a_tracked_setter_is_marked(self):
        out = _block([_insn(0, 2, "fcmovnbe", "st(0), st(1)", [_st(0), _st(1)])])
        self.assertIn("RECOMP_UNIMPL", out)

    def test_fcmov_reads_the_fcomi_flags(self):
        out = _block([_insn(0, 2, "fucomip", "st(0), st(1)", [_st(0), _st(1)]),
                      _insn(2, 2, "fcmovnbe", "st(0), st(1)", [_st(0), _st(1)])])
        self.assertIn("fp_top() = fp_st1(); /* fcmovnbe", out)
        self.assertNotIn("RECOMP_UNIMPL", out)


_SOURCE = r"""
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <stdio.h>
#include <math.h>
RECOMP_TLS uint32_t g_seh_ebp;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
ptrdiff_t g_xbox_mem_offset;
static uint8_t s_mem[0x1000];
@MACROS@
static void ficomp_case(void) { uint32_t eax = 0, ebp = 0; uint32_t _fa = 0; (void)eax; (void)ebp; (void)_fa;
@FICOMP@
}
static void fcmov_case(void) { uint32_t eax = 0, ebp = 0; uint32_t _fa = 0; int _cf = 0; (void)eax; (void)ebp; (void)_fa; (void)_cf;
@FCMOV@
}
static void fxtract_case(void) { uint32_t eax = 0, ebp = 0; (void)eax; (void)ebp;
@FXTRACT@
}
static void save_case(void) { uint32_t eax = 0, ebp = 0; (void)eax; (void)ebp;
@SAVE@
}
static void restore_case(void) { uint32_t eax = 0, ebp = 0; (void)eax; (void)ebp;
@RESTORE@
}
int main(void) {
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)s_mem;
    /* ficomp [0x100] (word 7): 5.0 < 7, the compare recorded, one pop. */
    *(int16_t *)(s_mem + 0x100) = 7;
    g_fp_top = 0; fp_push(9.0); fp_push(5.0); g_fp_cmp = 99;
    ficomp_case();
    if (g_fp_cmp != -1) return 10;
    if (fp_top() != 9.0) return 11;                 /* popped back to 9 */
    /* fucomip st(1) then fcmovnbe: the move happens when st0 was above st1. */
    g_fp_top = 0; fp_push(2.0); fp_push(1.0); fp_push(3.0);   /* st0 3, st1 1, st2 2 */
    fcmov_case();               /* 3 > 1 (then popped: st0 1, st1 2): above, so st0 = st1 */
    if (fp_top() != 2.0) return 20;
    g_fp_top = 0; fp_push(2.0); fp_push(1.0); fp_push(1.0);   /* equal: not above */
    fcmov_case();
    if (fp_top() != 1.0) return 21;
    /* fxtract: 12 = 1.5 * 2^3 -> st0 1.5, st1 3; 0 -> st0 0, st1 -inf. */
    g_fp_top = 0; fp_push(12.0); fxtract_case();
    if (fp_top() != 1.5 || fp_st1() != 3.0) return 30;
    g_fp_top = 0; fp_push(0.0); fxtract_case();
    if (fp_top() != 0.0 || !(isinf(fp_st1()) && fp_st1() < 0)) return 31;
    /* fnsave then frstor round-trips the stack, TOP, the control word and C3..C0,
     * and fnsave leaves the unit initialised. */
    g_fp_top = 0; fp_push(1.25); fp_push(-2.5); fp_push(1e300);
    g_fp_control_word = 0x027F; g_fp_cc = 0x4100;
    {
        int top = g_fp_top;
        double a = fp_top(), b = fp_st1(), c = fp_st(2);
        save_case();
        if (g_fp_control_word != 0x037F || g_fp_top != 0 || g_fp_cc != 0) return 40;
        if (*(uint16_t *)(s_mem + 0x200) != 0x027F) return 41;
        if (((*(uint16_t *)(s_mem + 0x204) >> 11) & 7) != top) return 42;
        fp_push(77.0);                               /* disturb it */
        restore_case();
        if (g_fp_top != top || g_fp_control_word != 0x027F || g_fp_cc != 0x4100) return 43;
        if (fp_top() != a || fp_st1() != b || fp_st(2) != c) return 44;
    }
    puts("OK");
    return 0;
}
"""


class RunTest(unittest.TestCase):
    def test_lifted_forms_compute_what_the_x87_does(self):
        cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler unavailable")
        ficomp = _block([_insn(0, 3, "ficomp", "word ptr [0x100]", [_mem(0x100, 2)])])
        fcmov = _block([_insn(0, 2, "fucomip", "st(0), st(1)", [_st(0), _st(1)]),
                        _insn(2, 2, "fcmovnbe", "st(0), st(1)", [_st(0), _st(1)])])
        fxtract = _block([_insn(0, 2, "fxtract")])
        save = _block([_insn(0, 3, "fnsave", "[0x200]", [_mem(0x200, 108)])])
        restore = _block([_insn(0, 3, "frstor", "[0x200]", [_mem(0x200, 108)])])
        src = (_SOURCE.replace("@MACROS@", "\n".join(FP_STACK_MACROS))
               .replace("@FICOMP@", ficomp).replace("@FCMOV@", fcmov)
               .replace("@FXTRACT@", fxtract).replace("@SAVE@", save)
               .replace("@RESTORE@", restore))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "drops.c"
            path.write_text(src)
            exe = Path(tmp) / "drops.exe"
            cmd = [cc, "-w", str(path), "-o", str(exe),
                   "-I", str(_ROOT / "templates" / "runtime"), "-I", str(_ROOT / "src")]
            if os.name != "nt":
                cmd.append("-lm")
            built = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr[-3000:] + "\n" + src)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual((ran.returncode, ran.stdout.strip()), (0, "OK"), src)


if __name__ == "__main__":
    unittest.main()
