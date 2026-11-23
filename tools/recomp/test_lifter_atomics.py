"""lock xadd / lock cmpxchg are InterlockedIncrement and friends.

Both used to fall through to a TODO comment: a reference count that never
moved and a compare-and-swap that never swapped. That was harmless while every
guest thread ran synchronously, and stopped being harmless once the runtime
began spawning real host threads for a title's workers.

Lowering them to a plain read-modify-write would not do either: a racing
sequence is the exact bug these instructions exist to prevent.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _mem_ecx():
    return Operand(type="mem", mem_base="ecx", mem_index=None, mem_scale=1,
                   mem_disp=0, mem_size=4)


def _lift(mnemonic, src_reg="eax"):
    insn = Instruction(0, 3, mnemonic, "dword ptr [ecx], %s" % src_reg, "0fc101")
    insn.operands = [_mem_ecx(), Operand(type="reg", reg=src_reg)]
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=[insn]))
    return chr(10).join(lifted)


class AtomicLiftTest(unittest.TestCase):
    def test_xadd_is_atomic_and_returns_the_old_value(self):
        generated = _lift("xadd", "edx")
        self.assertIn("RECOMP_ATOMIC_ADD32", generated)
        self.assertIn("_old", generated)
        self.assertNotIn("TODO", generated)

    def test_xchg_with_memory_is_atomic(self):
        # x86 locks xchg with memory; titles build locks on it.
        generated = _lift("xchg", "eax")
        self.assertIn("RECOMP_ATOMIC_XCHG32", generated)
        self.assertIn("eax = _tmp;", generated)

    def test_cmpxchg_is_a_real_compare_and_swap(self):
        generated = _lift("cmpxchg", "edx")
        self.assertIn("RECOMP_ATOMIC_CAS32", generated)
        # eax is only replaced when the comparand did not match.
        self.assertIn("if (_old != _cmp) eax = _old;", generated)
        self.assertNotIn("TODO", generated)

    def test_cmpxchg_snapshots_the_flags_before_touching_eax(self):
        # The flags a following jcc reads must come from the compare cmpxchg
        # did -- (eax - dest), in that order, so the cmp conditions apply --
        # not from eax after it may have been overwritten.
        generated = _lift("cmpxchg", "edx")
        cmp_at = generated.index("_fa = _cmp; _fb = _old;")
        eax_at = generated.index("if (_old != _cmp) eax = _old;")
        self.assertLess(cmp_at, eax_at)

    def test_lock_prefixed_forms_lift_the_same_way(self):
        for m in ("lock xadd", "lock cmpxchg"):
            with self.subTest(mnemonic=m):
                generated = _lift(m, "edx")
                self.assertNotIn("TODO", generated)
                self.assertIn("RECOMP_ATOMIC_", generated)


# ── The rest of the family (audit 2026-11-03, F2-F4) ──
#
# The LOCK prefix used to be part of the mnemonic, so "lock add" matched no
# table and fell through to RECOMP_UNIMPL: logged once, then skipped. The
# narrow xadd/cmpxchg, cmpxchg8b and the register form of xadd went the same
# way. And the flags after xadd were re-read from memory at the branch.

def _mem(reg="ecx", size=4):
    return Operand(type="mem", mem_base=reg, mem_index=None, mem_scale=1,
                   mem_disp=0, mem_size=size)


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(v):
    return Operand(type="imm", imm=v)


def _lift_ops(mnemonic, ops, op_str="", needs_cf=False):
    insn = Instruction(0x1000, 3, mnemonic, op_str, "")
    insn.operands = ops
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    lifted, _ = lift_basic_block(lifter, BasicBlock(start=0x1000,
                                                    instructions=[insn]))
    return chr(10).join(lifted)


class LockPrefixTest(unittest.TestCase):
    def test_the_prefix_is_a_flag_on_the_instruction(self):
        insn = Instruction(0, 3, "lock add", "dword ptr [ecx], eax", "f00101")
        self.assertEqual(insn.mnemonic, "add")
        self.assertTrue(insn.locked)
        self.assertEqual(insn.text, "lock add dword ptr [ecx], eax")
        plain = Instruction(0, 2, "add", "dword ptr [ecx], eax", "0101")
        self.assertFalse(plain.locked)

    def test_locked_read_modify_writes_are_atomic(self):
        cases = {
            "lock add": ([_mem(), _reg("eax")], "RECOMP_ATOMIC_ADD32"),
            "lock sub": ([_mem(), _reg("eax")], "RECOMP_ATOMIC_ADD32"),
            "lock or": ([_mem(), _reg("eax")], "RECOMP_ATOMIC_OR32"),
            "lock and": ([_mem(), _imm(0xFF)], "RECOMP_ATOMIC_AND32"),
            "lock xor": ([_mem(), _reg("edx")], "RECOMP_ATOMIC_XOR32"),
            "lock inc": ([_mem()], "RECOMP_ATOMIC_ADD32"),
            "lock dec": ([_mem()], "RECOMP_ATOMIC_ADD32"),
            "lock not": ([_mem()], "RECOMP_ATOMIC_XOR32"),
            "lock neg": ([_mem()], "RECOMP_ATOMIC_CAS32"),
            "lock bts": ([_mem(), _reg("eax")], "RECOMP_ATOMIC_OR32"),
            "lock btr": ([_mem(), _reg("eax")], "RECOMP_ATOMIC_AND32"),
            "lock btc": ([_mem(), _reg("eax")], "RECOMP_ATOMIC_XOR32"),
            "lock bts ": ([_mem(), _imm(5)], "RECOMP_ATOMIC_OR32"),
            "lock add ": ([_mem(size=1), _reg("al")], "RECOMP_ATOMIC_ADD8"),
            "lock inc ": ([_mem(size=2)], "RECOMP_ATOMIC_ADD16"),
        }
        for m, (ops, macro) in cases.items():
            with self.subTest(mnemonic=m):
                generated = _lift_ops(m.strip(), ops)
                self.assertIn(macro, generated)
                self.assertNotIn("TODO", generated)
                self.assertNotIn("RECOMP_UNIMPL", generated)

    def test_lock_sub_adds_the_negation_and_lock_dec_subtracts_one(self):
        generated = _lift_ops("lock sub", [_mem(), _reg("eax")])
        self.assertIn("(0u - _src) & 0xFFFFFFFFu", generated)
        self.assertIn("_fa = (_old - _src)", generated)
        generated = _lift_ops("lock dec", [_mem()])
        self.assertIn("uint32_t _src = 1u;", generated)
        # OF is published the way inc/dec always publish it (dec overflows
        # from 0x80000000, leaving 0x7FFFFFFF).
        self.assertIn("_fb = (_fa == 0x7FFFFFFFu);", generated)

    def test_locked_flags_come_from_the_value_the_atomic_returned(self):
        generated = _lift_ops("lock add", [_mem(), _reg("eax")], needs_cf=True)
        self.assertIn("_cf = (int)((((uint64_t)_old + (uint64_t)_src) >> 32) & 1);",
                      generated)
        self.assertIn("_fa = (_old + _src) & 0xFFFFFFFFu;", generated)
        generated = _lift_ops("lock bts", [_mem(), _reg("eax")], needs_cf=True)
        self.assertIn("_cf = (int)((_old >> ((eax) & 31)) & 1u);", generated)
        generated = _lift_ops("lock neg", [_mem()], needs_cf=True)
        self.assertIn("_cf = (int)(_old != 0);", generated)

    def test_unlocked_alu_on_memory_stays_a_plain_read_modify_write(self):
        generated = _lift_ops("add", [_mem(), _reg("eax")])
        self.assertNotIn("RECOMP_ATOMIC", generated)
        generated = _lift_ops("bts", [_mem(), _reg("eax")])
        self.assertNotIn("RECOMP_ATOMIC", generated)

    def test_a_locked_form_without_a_lift_is_still_reported_with_its_prefix(self):
        generated = _lift_ops("lock adc", [_mem(), _reg("eax")],
                              op_str="dword ptr [ecx], eax")
        self.assertIn('RECOMP_UNIMPL("lock adc dword ptr [ecx], eax"', generated)


class NarrowAtomicsTest(unittest.TestCase):
    def test_xadd_byte_and_word(self):
        generated = _lift_ops("xadd", [_mem(size=1), _reg("al")])
        self.assertIn("RECOMP_ATOMIC_ADD8", generated)
        self.assertIn("SET_LO8(eax, _old)", generated)
        self.assertIn("& 0xFFu", generated)
        generated = _lift_ops("lock xadd", [_mem(size=2), _reg("dx")])
        self.assertIn("RECOMP_ATOMIC_ADD16", generated)
        self.assertIn("SET_LO16(edx, _old)", generated)

    def test_cmpxchg_byte_and_word_use_the_narrow_accumulator(self):
        generated = _lift_ops("cmpxchg", [_mem(size=1), _reg("dl")])
        self.assertIn("uint32_t _cmp = (eax & 0xFFu);", generated)
        self.assertIn("RECOMP_ATOMIC_CAS8", generated)
        self.assertIn("if (_old != _cmp) SET_LO8(eax, _old)", generated)
        generated = _lift_ops("lock cmpxchg", [_mem(size=2), _reg("dx")])
        self.assertIn("uint32_t _cmp = (eax & 0xFFFFu);", generated)
        self.assertIn("RECOMP_ATOMIC_CAS16", generated)
        self.assertIn("SET_LO16(eax, _old)", generated)

    def test_cmpxchg8b(self):
        generated = _lift_ops("lock cmpxchg8b", [_mem(size=8)])
        self.assertIn("RECOMP_ATOMIC_CAS64", generated)
        self.assertIn("((uint64_t)edx << 32) | (uint64_t)eax", generated)
        self.assertIn("((uint64_t)ecx << 32) | (uint64_t)ebx", generated)
        self.assertIn("edx = (uint32_t)(_old >> 32)", generated)
        self.assertIn("_fa = (_old == _cmp); _fb = 1u;", generated)
        self.assertNotIn("RECOMP_UNIMPL", generated)

    def test_xadd_between_registers_is_plain_and_in_hardware_order(self):
        generated = _lift_ops("xadd", [_reg("eax"), _reg("eax")])
        self.assertNotIn("RECOMP_ATOMIC", generated)
        # SRC = DEST first, then DEST = sum: xadd eax, eax doubles eax.
        self.assertLess(generated.index("eax = _old;"), generated.index("eax = _fa;"))

    def test_cmpxchg_with_a_register_destination_is_plain(self):
        generated = _lift_ops("cmpxchg", [_reg("ecx"), _reg("edx")])
        self.assertNotIn("RECOMP_ATOMIC", generated)
        self.assertIn("if (_old == _cmp) ecx = (uint32_t)(edx) & 0xFFFFFFFFu;", generated)
        self.assertIn("if (_old != _cmp) eax = _old;", generated)


class AtomicFlagsTest(unittest.TestCase):
    """The branch after an atomic reads what the atomic saw, not memory."""

    @staticmethod
    def _translate(hexs):
        from . import config
        from .translator import FunctionTranslator
        image = bytes.fromhex(hexs)
        base = 0x10000
        config._install([config.Section(".text", base, len(image), 0,
                                        len(image), True)],
                        entry_point=base, kernel_thunk_addr=base,
                        origin="atomic-flags")
        db = {base: {"start": hex(base), "end": base + len(image),
                     "_addr": base, "size": len(image)}}
        return FunctionTranslator(image, db).translate_function(base, db[base])

    _TAIL = "31c0c3b801000000c3"   # xor eax,eax; ret / mov eax,1; ret

    def test_jz_after_xadd_reads_the_sum_not_memory(self):
        for spelling, code in (("xadd", "0fc101"), ("lock xadd", "f00fc101")):
            with self.subTest(spelling=spelling):
                c = self._translate(code + "7403" + self._TAIL)
                jz = [l for l in c.splitlines() if "/* je:" in l][0]
                self.assertIn("(_fa == 0)", jz)
                self.assertNotIn("MEM32(", jz)
                self.assertNotIn("_flags", jz)

    def test_jb_after_xadd_reads_the_carry_of_the_sum(self):
        c = self._translate("0fc101" + "7203" + self._TAIL)
        jb = [l for l in c.splitlines() if "/* jb:" in l][0]
        self.assertIn("_cf", jb)
        self.assertIn("_cf = (int)((((uint64_t)_old + (uint64_t)_src) >> 32) & 1);", c)

    def test_ordered_jcc_after_cmpxchg_compares_eax_with_dest(self):
        # jb: eax (the comparand) below dest -- _fa is eax, _fb is dest.
        c = self._translate("0fb111" + "7203" + self._TAIL)
        self.assertIn("_fa = _cmp; _fb = _old;", c)
        jb = [l for l in c.splitlines() if "/* jb:" in l][0]
        self.assertIn("CMP_B(_fa, _fb)", jb)
        # jl: the signed snapshot.
        c = self._translate("0fb111" + "7c03" + self._TAIL)
        jl = [l for l in c.splitlines() if "/* jl:" in l][0]
        self.assertIn("CMP_L(_fas, _fbs)", jl)

    def test_jz_after_cmpxchg8b(self):
        c = self._translate("0fc709" + "7403" + self._TAIL)
        jz = [l for l in c.splitlines() if "/* je:" in l][0]
        self.assertIn("(_fa == _fb)", jz)

    def test_lock_add_then_jz_reads_the_atomic_result(self):
        c = self._translate("f00101" + "7403" + self._TAIL)
        self.assertIn("RECOMP_ATOMIC_ADD32", c)
        jz = [l for l in c.splitlines() if "/* je:" in l][0]
        self.assertIn("(_fa == 0)", jz)
        self.assertNotIn("_flags", jz)
