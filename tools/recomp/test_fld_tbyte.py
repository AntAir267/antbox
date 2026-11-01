import unittest
from .disasm import Instruction, Operand
from .lifter import Lifter

class FldTbyteTest(unittest.TestCase):
    """fld and fstp of an 80-bit real aren't a float or a dropped store: the
    CRT's trig keeps pi/2 that way, and a float's read of it made MA2's atan2(y, 0)
    return 0."""

    def lift(self, mnemonic):
        instruction = Instruction(0, 6, mnemonic, 'tbyte ptr [0x4a03b2]', '')
        instruction.operands = [Operand(type='mem', mem_disp=0x4A03B2, mem_size=10)]
        return '\n'.join(Lifter().lift_instruction(instruction))

    def test_load(self):
        result = self.lift('fld')
        self.assertIn('fp_push(recomp_f80_load(', result)
        self.assertNotIn('MEMF', result)

    def test_store_pops_only_for_fstp(self):
        for mnemonic in ('fst', 'fstp'):
            with self.subTest(mnemonic=mnemonic):
                result = self.lift(mnemonic)
                self.assertIn('recomp_f80_store(', result)
                self.assertEqual('fp_pop()' in result, mnemonic == 'fstp')

if __name__ == '__main__':
    unittest.main()
