"""Guest load patches (--patches): the call after a listed `mov reg, [mem]`,
the refusals, and that nothing changes without it. Synthetic one-function
images, as in test_spin_waits."""

import json
import os
import tempfile

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator
from tools.recomp.patches import Patch, PatchConfig, PatchReport

BASE = 0x10000
DATA = 0x20000
D32 = DATA.to_bytes(4, "little").hex()

# mov eax, [DATA] ; ret
LOAD_EAX = f"a1{D32}c3"
# mov ebx, [DATA] ; ret
LOAD_EBX = f"8b1d{D32}c3"
# movzx ecx, byte [DATA] ; ret
LOAD_BYTE = f"0fb60d{D32}c3"
# mov [DATA], eax ; ret
STORE = f"a3{D32}c3"


# mov al, [DATA] ; ret
LOAD_AL = f"a0{D32}c3"
# mov cl, [DATA] ; ret
LOAD_CL = f"8a0d{D32}c3"
# mov ah, [DATA] ; ret
LOAD_AH = f"8a25{D32}c3"


def _translate(hexcode, patch=None, va=BASE, latch=None, kind="load_u32",
               value=2):
    image = bytes.fromhex(hexcode)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin="patch-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    report = None
    if patch is not None:
        report = PatchReport(PatchConfig([Patch(
            va, kind, "game.speed_units", "kmh", value, latch=latch)]))
        t.lifter.patches = report
    return t.translate_function(BASE, db[BASE]), report


def test_load_is_followed_by_the_call():
    code, report = _translate(LOAD_EAX, patch=True)
    assert ('eax = RECOMP_PATCH_U32("game.speed_units", "kmh", 0x2u, eax);'
            in code), code
    assert code.index("MEM32(0x20000)") < code.index("RECOMP_PATCH_U32")
    assert report.finish() == []


def test_latch_sites_use_the_latch_call():
    for latch, mode in (("set", 1), ("use", 2)):
        code, report = _translate(LOAD_EAX, patch=True, latch=latch)
        assert ('eax = RECOMP_PATCH_U32_LATCH("game.speed_units", "kmh", '
                f"0x2u, eax, {mode});") in code, code
        assert report.finish() == []


def test_the_register_is_the_loaded_one():
    code, report = _translate(LOAD_EBX, patch=True)
    assert 'ebx = RECOMP_PATCH_U32("game.speed_units", "kmh", 0x2u, ebx);' in code
    assert report.finish() == []


def test_nothing_changes_without_patches():
    code, _ = _translate(LOAD_EAX)
    assert "RECOMP_PATCH_U32" not in code


def test_other_addresses_are_left_alone():
    code, report = _translate(LOAD_EAX, patch=True, va=BASE + 0x100)
    assert "RECOMP_PATCH_U32" not in code
    assert any("no lifted body" in e for e in report.finish())


def test_a_byte_load_or_store_is_refused():
    for hexcode in (LOAD_BYTE, STORE):
        code, report = _translate(hexcode, patch=True)
        assert "RECOMP_PATCH_U32" not in code
        assert any("is not a 32-bit" in e for e in report.finish()), hexcode


def test_byte_load_changes_only_the_low_byte():
    code, report = _translate(LOAD_AL, patch=True, kind="load_u8", value=0)
    assert ('SET_LO8(eax, RECOMP_PATCH_U32("game.speed_units", "kmh", 0x0u, '
            "LO8(eax)));") in code, code
    assert code.index("MEM8(0x20000)") < code.index("RECOMP_PATCH_U32")
    assert report.finish() == []
    code, report = _translate(LOAD_CL, patch=True, kind="load_u8", value=1,
                              latch="use")
    assert ('SET_LO8(ecx, RECOMP_PATCH_U32_LATCH("game.speed_units", "kmh", '
            "0x1u, LO8(ecx), 2));") in code, code
    assert report.finish() == []
    code, _ = _translate(LOAD_AL)
    assert "RECOMP_PATCH" not in code


def test_byte_kind_refuses_other_shapes():
    # A 32-bit load, a zero-extending one, a store, the high byte register.
    for hexcode in (LOAD_EAX, LOAD_BYTE, STORE, LOAD_AH):
        code, report = _translate(hexcode, patch=True, kind="load_u8", value=0)
        assert "RECOMP_PATCH_U32" not in code, hexcode
        assert any("is not an 8-bit" in e for e in report.finish()), hexcode


# cmp byte [DATA], bl ; mov al, [DATA + 1] ; je +1 ; nop ; ret
CMP_MEM_BL = f"381d{D32}a0{(DATA + 1).to_bytes(4, 'little').hex()}740190c3"
# cmp byte [DATA], 0 ; jne +1 ; nop ; ret
CMP_MEM_IMM = f"803d{D32}00750190c3"
# cmp [DATA], ebx ; ret   /   cmp bl, [DATA] ; ret
CMP_MEM_EBX = f"391d{D32}c3"
CMP_BL_MEM = f"3a1d{D32}c3"
# cmp byte [DATA], bl ; sbb eax, eax ; ret  (the carry is read)
CMP_MEM_BL_SBB = f"381d{D32}19c0c3"


def test_cmp_kind_rewrites_the_compare_snapshot():
    """cmp_u8: the memory byte a compare reads becomes the value: the left
    snapshot is rewritten after the compare and before whatever branches on
    it, with a load in between or not."""
    for hexcode in (CMP_MEM_BL, CMP_MEM_IMM):
        code, report = _translate(hexcode, patch=True, kind="cmp_u8", value=0)
        want = ('_fa = RECOMP_PATCH_U32("game.speed_units", "kmh", 0x0u, _fa)'
                " & 0xFFu; _fas = (int32_t)(int8_t)(_fa);")
        assert want in code, code
        assert code.index("_fa = (uint32_t)(MEM8(0x20000))") < code.index(want)
        assert code.index(want) < code.index("if (CMP_"), code
        assert report.finish() == []
        plain, _ = _translate(hexcode)
        assert "RECOMP_PATCH" not in plain
        # Nothing else differs from the unpatched body.
        assert [l for l in code.splitlines() if "RECOMP_PATCH" not in l] \
            == plain.splitlines()
    # Where the function tracks the carry, the compare's carry is retaken.
    code, report = _translate(CMP_MEM_BL_SBB, patch=True, kind="cmp_u8", value=1)
    assert ("0x1u, _fa) & 0xFFu; _fas = (int32_t)(int8_t)(_fa); "
            "_cf = (int)(_fa < _fb);") in code, code
    assert report.finish() == []


def test_cmp_kind_refuses_other_shapes():
    for hexcode in (LOAD_AL, CMP_MEM_EBX, CMP_BL_MEM, STORE):
        code, report = _translate(hexcode, patch=True, kind="cmp_u8", value=0)
        assert "RECOMP_PATCH_U32" not in code, hexcode
        assert any("is not an 8-bit `cmp" in e for e in report.finish()), hexcode
    # No latch: a compare is not a value the title keeps.
    code, report = _translate(CMP_MEM_BL, patch=True, kind="cmp_u8", value=0,
                              latch="set")
    assert "RECOMP_PATCH_U32" not in code
    assert report.finish()


# cmp dword [DATA], 1 ; jg +1 ; nop ; ret
CMP_MEM32_IMM = f"833d{D32}017f0190c3"
# cmp [DATA], ebx ; mov eax, [DATA] ; je +1 ; nop ; ret
CMP_MEM32_REG = f"391d{D32}a1{D32}740190c3"
# mov byte [DATA], 1 ; ret   /   mov [DATA], bl ; ret
STORE_B_IMM = f"c605{D32}01c3"
STORE_B_BL = f"881d{D32}c3"
# mov dword [DATA], 1 ; ret
STORE_D_IMM = f"c705{D32}01000000c3"


def test_cmp_u32_rewrites_the_compare_snapshot():
    for hexcode in (CMP_MEM32_IMM, CMP_MEM32_REG):
        code, report = _translate(hexcode, patch=True, kind="cmp_u32", value=1)
        want = ('_fa = RECOMP_PATCH_U32("game.speed_units", "kmh", 0x1u, _fa);'
                " _fas = (int32_t)(_fa);")
        assert want in code, code
        assert code.index("_fa = (uint32_t)(MEM32(0x20000))") < code.index(want)
        assert code.index(want) < code.index("if (CMP_"), code
        assert report.finish() == []
        plain, _ = _translate(hexcode)
        assert [l for l in code.splitlines() if "RECOMP_PATCH" not in l] \
            == plain.splitlines()
    # A byte compare, a load, a store are not it; and the byte kind refuses
    # the 32-bit compare.
    for hexcode in (CMP_MEM_BL, LOAD_EAX, STORE, CMP_BL_MEM):
        code, report = _translate(hexcode, patch=True, kind="cmp_u32", value=1)
        assert "RECOMP_PATCH_U32" not in code, hexcode
        assert any("is not a 32-bit `cmp" in e for e in report.finish()), hexcode
    code, report = _translate(CMP_MEM32_IMM, patch=True, kind="cmp_u8", value=0)
    assert "RECOMP_PATCH_U32" not in code and report.finish()


def test_store_u8_stores_the_value_again():
    for hexcode in (STORE_B_IMM, STORE_B_BL):
        code, report = _translate(hexcode, patch=True, kind="store_u8", value=0)
        want = ('MEM8(0x20000) = (uint8_t)RECOMP_PATCH_U32("game.speed_units", '
                '"kmh", 0x0u, MEM8(0x20000));')
        assert want in code, code
        assert code.index("MEM8(0x20000) = ") < code.index(want)
        assert report.finish() == []
        plain, _ = _translate(hexcode)
        assert [l for l in code.splitlines() if "RECOMP_PATCH" not in l] \
            == plain.splitlines()
    for hexcode in (STORE_D_IMM, STORE, LOAD_AL, CMP_MEM_BL):
        code, report = _translate(hexcode, patch=True, kind="store_u8", value=0)
        assert "RECOMP_PATCH_U32" not in code, hexcode
        assert any("is not an 8-bit `mov byte" in e for e in report.finish()), hexcode
    code, report = _translate(STORE_B_IMM, patch=True, kind="store_u8", value=0,
                              latch="set")
    assert "RECOMP_PATCH_U32" not in code and report.finish()


# mov [DATA], ebx ; ret
STORE_D_EBX = f"891d{D32}c3"


def test_store_u32_stores_the_value_again():
    for hexcode in (STORE_D_IMM, STORE, STORE_D_EBX):
        code, report = _translate(hexcode, patch=True, kind="store_u32", value=1)
        want = ('MEM32(0x20000) = RECOMP_PATCH_U32("game.speed_units", '
                '"kmh", 0x1u, MEM32(0x20000));')
        assert want in code, code
        assert code.index("MEM32(0x20000) = ") < code.index(want)
        assert report.finish() == []
        plain, _ = _translate(hexcode)
        assert [l for l in code.splitlines() if "RECOMP_PATCH" not in l] \
            == plain.splitlines()
    for hexcode in (STORE_B_IMM, STORE_B_BL, LOAD_EAX, CMP_MEM32_IMM):
        code, report = _translate(hexcode, patch=True, kind="store_u32", value=1)
        assert "RECOMP_PATCH_U32" not in code, hexcode
        assert any("is not a 32-bit `mov dword" in e for e in report.finish()), hexcode
    code, report = _translate(STORE_D_IMM, patch=True, kind="store_u32", value=1,
                              latch="set")
    assert "RECOMP_PATCH_U32" not in code and report.finish()


def test_file_round_trip_and_bad_keys():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "patches.json")
        with open(path, "w") as f:
            json.dump({"patches": [{"va": "0x00010000",
                                    "setting": "game.speed_units",
                                    "when": "kmh", "value": 2}]}, f)
        cfg = PatchConfig.load(path)
        assert cfg.patches[0x10000].value == 2
        with open(path, "w") as f:
            json.dump({"patches": [{"va": "0x00010000", "kind": "load_u8",
                                    "setting": "graphics.motion_blur",
                                    "when": "0", "value": 0}]}, f)
        cfg = PatchConfig.load(path)
        assert cfg.patches[0x10000].kind == "load_u8"
        assert cfg.patches[0x10000].value == 0
        with open(path, "w") as f:
            json.dump({"patches": [{"va": "0x00010000", "kind": "cmp_u8",
                                    "setting": "graphics.crash_60fps",
                                    "when": "1", "value": 0}]}, f)
        assert PatchConfig.load(path).patches[0x10000].kind == "cmp_u8"
        with open(path, "w") as f:
            json.dump({"patches": [{"va": "0x00010000", "kind": "store_u32",
                                    "setting": "game.race_clock",
                                    "when": "1", "value": 1}]}, f)
        cfg = PatchConfig.load(path)
        assert cfg.patches[0x10000].kind == "store_u32"
        assert cfg.patches[0x10000].value == 1
        for bad in ({"va": "0x1", "setting": "Speed", "when": "kmh", "value": 2},
                    {"va": "0x1", "setting": "a.b", "when": "k mh", "value": 2},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": -1},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 1,
                     "kind": "store"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 256,
                     "kind": "load_u8"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "expr": "1",
                     "kind": "load_u8"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 256,
                     "kind": "cmp_u8"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 0,
                     "kind": "cmp_u8", "latch": "set"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 256,
                     "kind": "store_u8"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 0,
                     "kind": "store_u8", "latch": "use"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 1,
                     "kind": "store_u32", "latch": "set"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "expr": "1",
                     "kind": "store_u32"},
                    {"va": "0x1", "setting": "a.b", "when": "k",
                     "value": 0x100000000, "kind": "store_u32"},
                    {"va": "0x1", "setting": "a.b", "when": "k", "value": 1,
                     "kind": "cmp_u32", "latch": "set"}):
            with open(path, "w") as f:
                json.dump({"patches": [bad]}, f)
            try:
                PatchConfig.load(path)
            except ValueError:
                continue
            raise AssertionError(f"accepted {bad}")


# movss xmm0, [DATA] ; ret
LOAD_XMM0 = f"f30f1005{D32}c3"
# movss [eax + 0x48], xmm1 ; ret
STORE_XMM1 = "f30f114848c3"
# mov ecx, [eax + 8] ; ret
LOAD_ECX_EAX8 = "8b4808c3"
# mov [eax + 0x20], ecx ; ret
STORE_ECX = "894820c3"
OFF = "(640 - 480 * aspect) / 2"
C_OFF = '(640.0 - 480.0 * recomp_patch_var("aspect")) / 2.0'


def _translate_f(hexcode, kind, expr, latch=None, guard=None):
    image = bytes.fromhex(hexcode)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin="patch-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    report = PatchReport(PatchConfig([Patch(
        BASE, kind, "display.hud_fix", "1", None, latch=latch, expr=expr,
        guard=guard)]))
    t.lifter.patches = report
    return t.translate_function(BASE, db[BASE]), report


def test_float_load_into_xmm():
    code, report = _translate_f(LOAD_XMM0, "load_f32", "loaded + " + OFF)
    want = ('xmm0.f[0] = RECOMP_PATCH_F32("display.hud_fix", "1", 0, '
            f"((double)(xmm0.f[0]) + {C_OFF}), xmm0.f[0]);")
    assert want in code, code
    assert code.index("XMM_SCALAR(MEMF(0x20000))") < code.index(want)
    assert report.finish() == []


def test_float_load_x87_fld():
    # fld dword ptr [DATA] ; ret: the pushed value is the top of the stack
    fld = f"d905{D32}c3"
    code, report = _translate_f(fld, "load_f32", "loaded * 0.5")
    want = ('fp_top() = RECOMP_PATCH_F32("display.hud_fix", "1", 0, '
            "((double)(fp_top()) * 0.5), fp_top());")
    assert want in code, code
    assert code.index("fp_push(MEMF(0x20000))") < code.index(want)
    assert report.finish() == []
    # fld qword (8 bytes) is not a float load
    fld8 = f"dd05{D32}c3"
    code, report = _translate_f(fld8, "load_f32", "loaded")
    assert "RECOMP_PATCH_F32" not in code
    assert any("is not a" in e for e in report.finish())


def test_float_load_as_bits_with_guard_and_latch():
    code, report = _translate_f(
        LOAD_ECX_EAX8, "load_f32", "loaded * (4 / 3) / aspect", latch="use",
        guard=("edi", [0x60EA14, 0x60EA84]))
    want = ('if (edi == 0x60EA14u || edi == 0x60EA84u) { ecx = '
            'RECOMP_PATCH_F32_BITS("display.hud_fix", "1", 2, '
            "((double)(RECOMP_F32_OF_BITS(ecx)) * (4.0 / 3.0) / "
            'recomp_patch_var("aspect")), ecx); }')
    assert want in code, code
    assert report.finish() == []


def test_two_guarded_patches_share_a_site():
    image = bytes.fromhex(LOAD_ECX_EAX8)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="patch-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    mk = lambda expr, va: Patch(BASE, "load_f32", "display.hud_fix", "1", None,
                                expr=expr, guard=("edi", [va]))
    report = PatchReport(PatchConfig([mk("loaded * 2", 0x100), mk("loaded * 3", 0x200)]))
    t.lifter.patches = report
    code = t.translate_function(BASE, db[BASE])
    a = code.index("if (edi == 0x100u)")
    b = code.index("if (edi == 0x200u)")
    assert a < b and "* 2.0" in code[a:b] and "* 3.0" in code[b:], code
    assert report.finish() == []
    for bad in ([Patch(BASE, "load_f32", "a.b", "1", None, expr="loaded"),
                 mk("loaded", 0x100)],
                [Patch(BASE, "load_u32", "a.b", "1", 2),
                 Patch(BASE, "load_u32", "a.b", "1", 3)]):
        try:
            PatchConfig(bad)
        except ValueError:
            continue
        raise AssertionError("accepted an unguarded pair")


def test_mark_calls_the_host_and_changes_nothing():
    image = bytes.fromhex(STORE)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="patch-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    plain = t.translate_function(BASE, db[BASE])
    p = Patch(BASE, "mark", "", "", None)
    p.name = "stage_render"
    report = PatchReport(PatchConfig([p]))
    t = FunctionTranslator(image, db)
    t.lifter.patches = report
    code = t.translate_function(BASE, db[BASE])
    line = 'RECOMP_PATCH_MARK("stage_render"); /* patch 0x00010000, mark */'
    assert line in code and report.finish() == []
    strip = lambda c: [l.strip() for l in c.splitlines() if l.strip() != line]
    assert strip(code) == strip(plain)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "patches.json")
        for doc, ok in (({"va": "0x10000", "kind": "mark", "name": "a_b"}, True),
                        ({"va": "0x10000", "kind": "mark", "name": "A"}, False),
                        ({"va": "0x10000", "kind": "mark", "name": "a", "when": "1"}, False),
                        ({"va": "0x10000", "setting": "a.b", "when": "!off", "kind": "load_f32", "expr": "loaded"}, True)):
            with open(path, "w") as f:
                json.dump({"patches": [doc]}, f)
            try:
                PatchConfig.load(path)
                assert ok, doc
            except ValueError:
                assert not ok, doc


def test_float_store_rewrites_the_stored_value():
    code, report = _translate_f(STORE_XMM1, "store_f32", "loaded + " + OFF,
                                latch="set")
    want = ('MEMF(eax + 0x48) = RECOMP_PATCH_F32("display.hud_fix", "1", 1, '
            f"((double)(MEMF(eax + 0x48)) + {C_OFF}), MEMF(eax + 0x48));")
    assert want in code, code
    assert code.index("MEMF(eax + 0x48) = xmm1.f[0];") < code.index(want)
    code, report = _translate_f(STORE_ECX, "store_f32", "loaded")
    assert ('MEM32(eax + 0x20) = RECOMP_PATCH_F32_BITS("display.hud_fix", '
            '"1", 0, ((double)(RECOMP_F32_OF_BITS(MEM32(eax + 0x20)))), '
            "MEM32(eax + 0x20));") in code, code
    assert report.finish() == []


def test_float_compare_rewrites_the_memory_operand():
    # comiss xmm0, [DATA] ; ret
    code, report = _translate_f(f"0f2f05{D32}c3", "cmp_f32", "loaded * 2 + 5")
    want = ('_fcb = RECOMP_PATCH_F32("display.hud_fix", "1", 0, '
            "((double)(_fcb) * 2.0 + 5.0), _fcb);")
    assert want in code, code
    assert code.index("_fcb = MEMF(0x20000);") < code.index(want)
    assert report.finish() == []
    # ucomiss too
    code, report = _translate_f(f"0f2e05{D32}c3", "cmp_f32", "60")
    assert '_fcb = RECOMP_PATCH_F32("display.hud_fix", "1", 0, ' in code, code
    assert report.finish() == []
    # a register compare, or a load, is not this kind
    for hexcode in ("0f2fc1c3", LOAD_XMM0):
        code, report = _translate_f(hexcode, "cmp_f32", "loaded")
        assert "RECOMP_PATCH_F32" not in code
        assert any("is not a" in e for e in report.finish()), hexcode


def test_float_kinds_refuse_other_shapes():
    for hexcode, kind in ((STORE_XMM1, "load_f32"), (LOAD_XMM0, "store_f32"),
                          (LOAD_BYTE, "load_f32"), (STORE, "load_f32")):
        code, report = _translate_f(hexcode, kind, "loaded")
        assert "RECOMP_PATCH_F32" not in code
        assert any("is not a" in e for e in report.finish()), (hexcode, kind)


def test_expressions():
    from tools.recomp.patches import compile_expr
    c, names = compile_expr("loaded + 320 * (1 - (4 / 3) / aspect)")
    assert names == ["aspect"]
    # The patched values equal the title's own in 4:3, to the bit.
    for expr, loaded in (("loaded + 320 * (1 - (4 / 3) / aspect)", 0.0),
                         ("loaded * (4 / 3) / aspect", 1.0),
                         ("loaded + " + OFF, 30.0), ("loaded - " + OFF, 610.0)):
        c, _ = compile_expr(expr)
        py = c.replace('recomp_patch_var("aspect")', "(4.0 / 3.0)").replace(
            "{loaded}", repr(loaded))
        assert eval(py) == loaded, (expr, eval(py))
    c, names = compile_expr(
        "f32[esp + 0x30] + (loaded - f32[esp+0x30]) * (4 / 3) / aspect")
    assert names == ["aspect"]
    assert c == ('((double)MEMF(esp + 0x30) + ({loaded} - (double)MEMF(esp + 0x30))'
                 ' * (4.0 / 3.0) / recomp_patch_var("aspect"))'), c
    assert compile_expr("f32[eax] - f32[ebp - 0x8]")[0] == \
        "((double)MEMF(eax) - (double)MEMF(ebp - 0x8))"
    for bad in ("a b", "1 +", "f(x)", "x; y", "(1", '"a"', "", "1 ** 2",
                "x ? 1 : 2", "a[0]", "f32[xmm0]", "f32[esp + 48]",
                "f32[esp + eax]", "f32[0x1000]", "f32"):
        try:
            compile_expr(bad)
        except ValueError:
            continue
        raise AssertionError(f"accepted {bad!r}")


def test_float_file_keys():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "patches.json")
        good = {"va": "0x00010000", "kind": "load_f32", "setting": "a.b",
                "when": "1", "expr": "loaded * 2",
                "if": {"reg": "edi", "in": ["0x60EA14"]}}
        with open(path, "w") as f:
            json.dump({"patches": [good]}, f)
        p = PatchConfig.load(path).patches[0x10000]
        assert p.guard == ("edi", [0x60EA14]) and p.names == []
        for change in ({"value": 2}, {"expr": 3}, {"expr": "f(1)"},
                       {"if": {"reg": "xmm0", "in": ["0x1"]}},
                       {"if": {"reg": "edi", "in": []}},
                       {"kind": "load_u32"}):
            with open(path, "w") as f:
                json.dump({"patches": [dict(good, **change)]}, f)
            try:
                PatchConfig.load(path)
            except (ValueError, KeyError):
                continue
            raise AssertionError(f"accepted {change}")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
