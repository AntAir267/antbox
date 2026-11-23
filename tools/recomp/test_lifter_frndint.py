"""FRNDINT must round under the guest's x87 rounding control, not the host's.

It was lifted as `rint(fp_top())`, which rounds in the HOST's mode -- nearest,
always, because the guest's `fldcw` only ever reaches g_fp_control_word. MSVC's
floor() and ceil() are `_ctrlfp(RC=down/up); _frnd(); _ctrlfp(old)`, and
_frnd is a bare frndint, so both of them rounded to nearest: floor of 2.7 was 3.
A title that indexes a keyframe table with floor() and ceil() then reads one
key past the end once per animation loop.

The emitted text is checked here, and the helper is compiled and run: a text
check cannot tell a correct nearest-even from one that rounds halves up.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction
from .lifter import Lifter

_ROOT = Path(__file__).resolve().parents[2]


class FrndintLiftTest(unittest.TestCase):
    def test_frndint_reads_the_guest_control_word(self):
        insn = Instruction(0x0017EEDB, 2, "frndint", "", "")
        insn.operands = []
        out = "\n".join(Lifter().lift_instruction(insn))
        self.assertIn("fp_top() = recomp_frndint(fp_top(), g_fp_control_word);", out)
        self.assertNotIn("rint(fp_top())", out.replace("recomp_frndint(fp_top()", ""))


class FrndintRuntimeTest(unittest.TestCase):
    def test_each_rounding_control_rounds_its_own_way(self):
        cc = next(filter(None, map(shutil.which, ("cc", "gcc", "clang"))), None)
        if not cc:
            self.skipTest("no C compiler available")
        source = r'''
#include <stdint.h>
#include <math.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

#define NEAR 0x037Fu   /* the x87 default: RC=00 */
#define DOWN 0x077Fu   /* RC=01, what floor() loads */
#define UP   0x0B7Fu   /* RC=10, what ceil() loads */
#define CHOP 0x0F7Fu   /* RC=11 */

int main(void) {
    /* The defect: floor and ceil of a fraction above one half. */
    if (recomp_frndint(59.6, DOWN) != 59.0) return __LINE__;
    if (recomp_frndint(59.6, UP)   != 60.0) return __LINE__;
    if (recomp_frndint(59.4, UP)   != 60.0) return __LINE__;
    if (recomp_frndint(-2.5, DOWN) != -3.0) return __LINE__;
    if (recomp_frndint(-2.5, UP)   != -2.0) return __LINE__;
    if (recomp_frndint(-2.7, CHOP) != -2.0) return __LINE__;
    if (recomp_frndint(2.7, CHOP)  !=  2.0) return __LINE__;
    /* Nearest-even: halves go to the even neighbour. */
    if (recomp_frndint(2.5, NEAR)  !=  2.0) return __LINE__;
    if (recomp_frndint(3.5, NEAR)  !=  4.0) return __LINE__;
    if (recomp_frndint(-1.5, NEAR) != -2.0) return __LINE__;
    if (recomp_frndint(2.4, NEAR)  !=  2.0) return __LINE__;
    /* Integral values, huge values and non-finite values pass through. */
    if (recomp_frndint(60.0, DOWN) != 60.0) return __LINE__;
    if (recomp_frndint(60.0, UP)   != 60.0) return __LINE__;
    if (recomp_frndint(1e300, UP) != 1e300) return __LINE__;
    if (!isnan(recomp_frndint((double)NAN, DOWN))) return __LINE__;
    if (recomp_frndint((double)INFINITY, CHOP) != (double)INFINITY) return __LINE__;
    /* A zero result keeps the operand's sign. */
    if (!signbit(recomp_frndint(-0.3, NEAR))) return __LINE__;
    if (!signbit(recomp_frndint(-0.7, UP)))   return __LINE__;
    if (signbit(recomp_frndint(0.3, DOWN)))   return __LINE__;
    /* FIST shares the rounding, and keeps its integer indefinite. */
    if (recomp_fist(59.6, DOWN, 32) != 59) return __LINE__;
    if (recomp_fist(59.6, NEAR, 32) != 60) return __LINE__;
    if (recomp_fist((double)NAN, NEAR, 32) != INT32_MIN) return __LINE__;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="frndint-runtime-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe)]
                    + ([] if os.name == "nt" else ["-lm"]),
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode,
                             f"check on line {ran.returncode} failed")

    def test_nearest_parity_is_the_old_fmod_bit_for_bit(self):
        """The half-to-even test reads lo's parity from (int64_t)lo & 1, not
        fmod(lo, 2.0) (BO-282.2): the same bits on every case below, the
        old helper compiled beside the new."""
        cc = next(filter(None, map(shutil.which, ("cc", "gcc", "clang"))), None)
        if not cc:
            self.skipTest("no C compiler available")
        source = r'''
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

static double old_frndint(double value, uint16_t control) {
    double rounded;
    if (!isfinite(value)) return value;
    switch((control>>10)&3) {
    case 1: rounded=floor(value); break;
    case 2: rounded=ceil(value); break;
    case 3: rounded=trunc(value); break;
    default: {
        double lo=floor(value), fraction=value-lo;
        rounded=lo;
        if(fraction>0.5 || (fraction==0.5 && fmod(lo,2.0)!=0.0)) rounded=lo+1.0;
        break;
    }
    }
    return rounded == 0.0 ? copysign(0.0, value) : rounded;
}

static int same(double a, double b) { uint64_t x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); return x == y; }

static int check(double v) {
    static const uint16_t cw[4] = { 0x037F, 0x077F, 0x0B7F, 0x0F7F };
    int k;
    for (k = 0; k < 4; k++)
        if (!same(recomp_frndint(v, cw[k]), old_frndint(v, cw[k]))) return 0;
    return 1;
}

int main(void) {
    static const double edge[] = {
        0.5, 1.5, 2.5, 3.5, -0.5, -1.5, -2.5, -3.5, 0.0, -0.0, 0.49999999999999994,
        -0.49999999999999994, 1e-300, -1e-300, 4.9e-324, -4.9e-324,
        4503599627370495.5, -4503599627370495.5,          /* 2^52 - 0.5: the last halves */
        4503599627370494.5, -4503599627370494.5,
        4503599627370496.0, 4503599627370497.0,           /* 2^52, 2^52 + 1 */
        9007199254740991.0, 9007199254740992.0, 9007199254740994.0,   /* 2^53 - 1, 2^53, + 2 */
        -9007199254740991.0, -9007199254740992.0, 1e300, -1e300,
        9.2233720368547758e18, -9.2233720368547758e18, 1.8e19, -1.8e19,
        (double)INFINITY, -(double)INFINITY, (double)NAN,
    };
    unsigned i;
    uint64_t r = 0x9E3779B97F4A7C15ull;
    for (i = 0; i < sizeof edge / sizeof edge[0]; i++)
        if (!check(edge[i])) return 100 + (int)i;
    /* Halves and near-halves at every magnitude up to 2^53, both signs. */
    for (i = 0; i < 2000000; i++) {
        double v;
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        v = ldexp((double)(r >> 11), -(int)(r % 64u));          /* below 2^53 */
        if (i & 1) v = floor(v) + 0.5;
        if (i & 2) v = -v;
        if (i % 5 == 0) v = nextafter(v, (i & 4) ? INFINITY : -INFINITY);
        if (!check(v)) return 3;
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="frndint-parity-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe)]
                    + ([] if os.name == "nt" else ["-lm"]),
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode, f"case {ran.returncode} differs")


if __name__ == "__main__":
    unittest.main()
