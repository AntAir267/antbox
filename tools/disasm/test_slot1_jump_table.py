"""A jump table indexed from slot 1 (upstream f820d2f, in antbox's form).

MSVC's memcpy and memmove dispatch their lead bytes through

    jmp  dword ptr [eax*4 + LeadUpVec - 4]      ; eax 1..3

so the slot at the displacement is never read: it is the tail of whatever
precedes the table. Counted from there the table has no entries, and
resync_jump_tables left it unmeasured, so its pointers were decoded as
instructions and the function ended at its own dispatch (upstream:
MechAssault's memmove at 0x001D0580 ended at 0x001D05E4).

Fixtures are synthetic: hand-assembled bytes, no game data.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.test_downward_jump_table import BASE, _engine  # noqa: E402


def _leadup(n_slots, slot0=b"\xcc\xcc\xcc\xcc"):
    """BASE+0: mov eax, [esp+4]; BASE+4: jmp [eax*4 + DISP]; padding to
    BASE+16 = DISP, whose four bytes are slot 0 (not a pointer); the table
    from DISP+4; then the arms, `mov eax, k ; ret`, 6 bytes each."""
    disp = BASE + 16
    table = disp + 4
    arms_at = table + 4 * n_slots
    arms = [arms_at + 6 * k for k in range(n_slots)]
    code = b"\x8b\x44\x24\x04" + b"\xff\x24\x85" + struct.pack("<I", disp)
    code += b"\xcc" * (disp - BASE - len(code))
    code += slot0
    code += struct.pack("<%dI" % n_slots, *arms)
    code += b"".join(b"\xb8" + struct.pack("<I", k) + b"\xc3" for k in range(n_slots))
    code += b"\xcc" * 16
    return code, disp, table, arms


def test_table_after_an_unread_slot_0_is_measured():
    data, disp, table, arms = _leadup(3)
    engine = _engine(data)
    assert engine.jump_tables.get(table) == table + 12, {
        hex(k): hex(v) for k, v in engine.jump_tables.items()}
    assert not [a for a in engine.instructions if table <= a < table + 12]
    for arm in arms:
        assert arm in engine.instructions, hex(arm)


def test_two_slots_after_slot_0_are_not_enough():
    data, disp, table, arms = _leadup(2)
    engine = _engine(data)
    assert table not in engine.jump_tables
    assert disp not in engine.jump_tables


def test_far_apart_targets_are_not_a_table():
    # Pointers into the section but far from each other: data, not arms.
    data, disp, table, arms = _leadup(3)
    far = bytearray(data)
    struct.pack_into("<I", far, table + 8 - BASE, BASE + 0x800)
    far += b"\xcc" * 0x900
    engine = _engine(bytes(far))
    assert engine.jump_tables.get(table) != table + 12
