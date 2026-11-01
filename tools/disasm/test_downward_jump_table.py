"""A jump table indexed downward from its displacement.

MSVC's memcpy and memmove finish their last dwords through a table that the
index walks down from:

    neg  ecx                     ; 0, -1, -2, -3
    jmp  dword ptr [ecx*4 + UnwindVec]

The table is the slots below the displacement, ending with the slot at the
displacement itself. Upward, the next word is already the first arm's code,
so the upward count sees one entry and resync_jump_tables used to leave the
table unmeasured. Its pointers were then decoded as instructions. On the PAL
Burnout 3 one of them read as `je`, which cut memcpy short of its arms: an
unresolved indirect jump during XAPI init.

Fixtures are synthetic: hand-assembled bytes, no game data.
"""
import os
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402

BASE = 0x10000


class Image:
    base_address = BASE
    entry_point = BASE

    def __init__(self, data):
        self.data = data
        self.image_size = len(data)
        self.section = SimpleNamespace(name=".text", virtual_addr=BASE,
                                       virtual_size=len(data), executable=True)
        self.sections = [self.section]

    def get_section_at_va(self, addr):
        return self.section if BASE <= addr < BASE + len(self.data) else None

    def get_section_data(self, section):
        return self.data

    def read_bytes_at_va(self, addr, size):
        return self.data[addr - BASE:addr - BASE + size]

    def read_u32_at_va(self, addr):
        off = addr - BASE
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]


def _engine(data):
    image = Image(data)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.section)
    engine.resync_jump_tables()
    return engine


def _unwind(n_slots):
    """The tail of a memcpy: the dispatch, the table of n_slots below and at
    its displacement, then the arms. Returns (data, displacement, table start,
    arms in slot order from the lowest).

        BASE+0:   mov ecx, [esp+4]
        BASE+4:   neg ecx
        BASE+6:   jmp dword ptr [ecx*4 + DISP]
        BASE+13:  int3 x3                     so the table is dword aligned
        BASE+16:  n_slots dwords; the last one is at DISP
        DISP+4:   the arms, `mov eax, k ; ret`, 6 bytes each
    """
    head = b"\x8b\x4c\x24\x04" + b"\xf7\xd9"
    start = BASE + 16
    disp = start + 4 * (n_slots - 1)
    arms_at = disp + 4
    arms = [arms_at + 6 * k for k in range(n_slots)]
    code = head + b"\xff\x24\x8d" + struct.pack("<I", disp)
    code += b"\xcc" * (start - BASE - len(code))
    # The lowest slot is the deepest index (-(n-1)); the arm for index 0 is
    # first after the table, as in the CRT.
    code += struct.pack("<%dI" % n_slots, *reversed(arms))
    code += b"".join(b"\xb8" + struct.pack("<I", k) + b"\xc3" for k in range(n_slots))
    code += b"\xcc" * 16
    return code, disp, start, list(reversed(arms))


def test_downward_table_is_measured_from_below():
    data, disp, start, slots = _unwind(4)
    engine = _engine(data)
    assert engine.jump_tables.get(start) == disp + 4, {
        hex(k): hex(v) for k, v in engine.jump_tables.items()}
    # No instruction is left over the table's bytes.
    assert not [a for a in engine.instructions if start <= a < disp + 4]
    # The arms behind it are decoded where they are.
    for arm in slots:
        assert arm in engine.instructions, hex(arm)


def test_displacement_names_the_whole_table():
    data, disp, start, slots = _unwind(4)
    engine = _engine(data)
    assert engine.jump_table_entries(disp) == slots
    assert engine.jump_table_entries(start) == slots


def test_two_slots_are_not_enough():
    # One slot below the displacement and the one at it: too short to tell
    # from code that only looks like pointers, so it is left alone, as an
    # upward table that short is.
    data, disp, start, slots = _unwind(2)
    engine = _engine(data)
    assert start not in engine.jump_tables
    assert disp not in engine.jump_tables
    assert engine.jump_table_entries(disp) == []
