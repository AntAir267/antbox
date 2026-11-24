"""One-operand mul/imul/div/idiv at the operand's width.

`imul bl` is AX = AL * BL; the lifter emitted the 32-bit form, EDX:EAX =
EAX * EBX, which used EAX's upper bits and wrote EDX. Burnout 3's dotted
address parser (0x214A30: `mov bl, 0xa; imul bl` per digit, DL as each
octet's starting value) then turned 127.0.0.3 into 127.10.0.3, and the voice
peer of an online game was sent to that (BO-134). 16-bit forms work on
DX:AX, 8-bit div/idiv on AX (quotient AL, remainder AH).

The emission checks are the control: the unfixed lifter writes `edx =` for
every narrow form.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _emit(mnemonic, reg):
    ops = [Operand(type="reg", reg=reg)]
    insn = Instruction(0, 2, mnemonic, reg, "", operands=ops)
    return " ".join(Lifter().lift_instruction(insn))


class NarrowMulDivTest(unittest.TestCase):
    def test_8bit_forms_leave_edx_alone(self):
        for m in ("mul", "imul", "div", "idiv"):
            with self.subTest(m=m):
                out = _emit(m, "bl")
                self.assertNotIn("edx =", out)
                self.assertNotIn("SET_LO16(edx", out)
                self.assertIn("eax", out)

    def test_8bit_imul_is_signed_on_al(self):
        # upstream 47ea2b63's product: (int8_t)eax is AL, as LO8(eax) was.
        out = _emit("imul", "bl")
        self.assertIn("(int64_t)(int8_t)eax * (int64_t)(int8_t)(LO8(ebx))", out)
        self.assertIn("SET_LO16(eax", out)

    def test_16bit_forms_use_dx_ax(self):
        for m in ("mul", "imul", "div", "idiv"):
            with self.subTest(m=m):
                out = _emit(m, "cx")
                self.assertIn("SET_LO16(edx", out)
                self.assertNotIn("edx = (uint32_t)", out)

    def test_32bit_forms_unchanged(self):
        # The same high word, spelt through the unsigned type; the product
        # also records CF = OF ("did not fit") in _fa now.
        for m in ("imul", "mul"):
            out = _emit(m, "ebx")
            self.assertIn("edx = (uint32_t)((uint64_t)_r >> 32)", out)
            self.assertIn("_fa = (", out)


if __name__ == "__main__":
    unittest.main()
