"""Tests for calls to functions replaced through manual dispatch."""

import tempfile

from tools.recomp.disasm import BasicBlock, Instruction, Operand
from tools.recomp.lifter import Lifter, lift_basic_block
from tools.recomp.translator import BatchTranslator, _fixup_icall_esp_save


TARGET = 0x001E9100


def _direct_call(target=TARGET):
    insn = Instruction(0x00120000, 5, "call", f"0x{target:x}", "e800000000")
    insn.call_target = target
    return insn


def _tail_jump(target=TARGET):
    insn = Instruction(0x00120000, 5, "jmp", f"0x{target:x}", "e900000000")
    insn.jump_target = target
    return insn


def test_selected_direct_call_uses_manual_lookup():
    lifted = Lifter(manual_functions={TARGET}).lift_instruction(_direct_call())
    generated = "\n".join(lifted)

    assert "PUSH32(esp, 0x00120005u);" in generated
    assert "RECOMP_ICALL_SAFE(0x001E9100u, _icall_esp)" in generated
    assert "sub_001E9100();" not in generated


def test_unselected_direct_call_remains_direct():
    lifted = Lifter().lift_instruction(_direct_call())
    generated = "\n".join(lifted)

    # A direct call goes through RECOMP_ABI_CALL, which expands to a plain
    # (fn)() unless -DRECOMP_ABI_CHECK is set. What matters here is that it
    # names the function rather than routing through the manual lookup.
    assert "sub_001E9100" in generated
    assert "RECOMP_ICALL_SAFE" not in generated
    assert "RECOMP_ICALL_SAFE" not in generated


def test_selected_tail_jump_uses_manual_lookup():
    lifter = Lifter(manual_functions={TARGET})
    lifter.func_start = 0x00120000
    lifter.func_end = 0x00120100

    generated = "\n".join(lifter.lift_instruction(_tail_jump()))

    assert "RECOMP_ITAIL(0x001E9100u)" in generated
    assert "sub_001E9100();" not in generated


def test_selected_tail_jump_keeps_exit_trace():
    lifter = Lifter(manual_functions={TARGET})
    lifter.func_start = 0x00120000
    lifter.func_end = 0x00120100
    lifter.trace_exit_name = "sub_00120000"

    generated = "\n".join(lifter.lift_instruction(_tail_jump()))

    assert 'RECOMP_TRACE_ESP("sub_00120000", "tail 0x001E9100")' in generated


def test_selected_conditional_tail_uses_manual_lookup():
    compare = Instruction(0x00120000, 2, "cmp", "eax, ecx", "39c8")
    compare.operands = [
        Operand(type="reg", reg="eax"),
        Operand(type="reg", reg="ecx"),
    ]
    jump = Instruction(
        0x00120002, 6, "je", f"0x{TARGET:x}", "0f8400000000")
    jump.jump_target = TARGET
    lifter = Lifter(manual_functions={TARGET})
    lifter.func_start = 0x00120000
    lifter.func_end = 0x00120100

    generated, _ = lift_basic_block(
        lifter, BasicBlock(start=0x00120000, instructions=[compare, jump]))
    output = "\n".join(generated)

    assert "RECOMP_ITAIL(0x001E9100u)" in output
    assert "sub_001E9100();" not in output


def test_direct_manual_call_saves_esp_before_argument_pushes():
    lines = [
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0x00120005u); "
        "RECOMP_ICALL_SAFE(0x001E9100u, _icall_esp); "
        "/* manual call 0x001E9100 */",
    ]

    fixed = _fixup_icall_esp_save(lines)
    save = next(i for i, line in enumerate(fixed) if "_icall_esp = g_esp" in line)
    argument = next(i for i, line in enumerate(fixed) if line.strip() == "PUSH32(esp, ecx);")

    assert save < argument


def test_split_translation_passes_manual_set_to_lifter():
    class FakeTranslator:
        def __init__(self):
            self.owned_function_starts = set()
            self.lifter = Lifter()
            self.seen_manual = None

        def translate_function(self, addr, func_info):
            self.seen_manual = self.lifter.manual_functions
            return f"void {func_info['name']}(void) {{}}"

    batch = BatchTranslator.__new__(BatchTranslator)
    batch.translator = FakeTranslator()
    functions = [
        (0x00120000, {"name": "sub_00120000"}),
        (TARGET, {"name": "sub_001E9100"}),
    ]

    with tempfile.TemporaryDirectory() as output_dir:
        batch.translate_batch_split(
            functions, output_dir, manual={TARGET})

    assert batch.translator.seen_manual == {TARGET}


def test_wrapped_direct_call_names_the_wrapper():
    # The body is renamed sub_X_gen in func_db; a direct call must still
    # reach the project's sub_X, or the wrap never sees it.
    lifter = Lifter(
        func_db={TARGET: {"name": "sub_001E9100_gen"}}, wrapped_functions={TARGET})
    generated = "\n".join(lifter.lift_instruction(_direct_call()))

    assert "RECOMP_ABI_CALL(0x001E9100u, sub_001E9100);" in generated
    assert "sub_001E9100_gen" not in generated


def test_split_translation_declares_and_dispatches_wrapper():
    class FakeTranslator:
        def __init__(self):
            self.owned_function_starts = set()
            self.lifter = Lifter(func_db={TARGET: {"name": "sub_001E9100_gen"}})

        def translate_function(self, addr, func_info):
            if addr == 0x00120000:
                self.lifter._call_target_name(TARGET)
            return f"void {func_info['name']}(void) {{}}"

    batch = BatchTranslator.__new__(BatchTranslator)
    batch.translator = FakeTranslator()
    functions = [
        (0x00120000, {"name": "sub_00120000"}),
        (TARGET, {"name": "sub_001E9100_gen"}),
    ]

    with tempfile.TemporaryDirectory() as output_dir:
        stats = batch.translate_batch_split(functions, output_dir, wrapped={TARGET})
        header = open(f"{output_dir}/recomp_funcs.h").read()
        dispatch = open(f"{output_dir}/recomp_dispatch.c").read()
        bodies = "".join(
            open(f).read() for f in stats["files"] if "recomp_0" in f)

    assert "void sub_001E9100_gen(void)" in bodies
    assert "void sub_001E9100(void);  /* 0x001E9100 */" in header
    assert "(recomp_func_t)sub_001E9100 }" in dispatch
    assert "sub_001E9100_gen }" not in dispatch
    assert stats["unresolved_stubs"] == 0


def test_wrapped_tail_jump_names_the_wrapper():
    lifter = Lifter(
        func_db={TARGET: {"name": "sub_001E9100_gen"}}, wrapped_functions={TARGET})
    lifter.func_start = 0x00120000
    lifter.func_end = 0x00120100

    generated = "\n".join(lifter.lift_instruction(_tail_jump()))

    assert "sub_001E9100(); return;" in generated
    assert "sub_001E9100_gen" not in generated


def test_wrapped_guarded_icall_arm_names_the_wrapper():
    # A recorded indirect-call site whose one target is wrapped: the guarded
    # arm calls the wrapper, and the fallback still reaches it through the
    # dispatch table (which test_split_translation_declares_and_dispatches_wrapper
    # covers).
    from tools.recomp import test_icall_guarded as g
    from tools.recomp.translator import FunctionTranslator

    img, db = g._image(), g._db()
    db[g.A]["name"] = f"sub_{g.A:08X}_gen"
    tr = FunctionTranslator(img, db, icall_sites={g.SITE: [g.A]})
    tr.lifter.wrapped_functions = {g.A}
    c = tr.translate_function(g.BASE, db[g.BASE])

    assert f"RECOMP_ABI_CALL(0x{g.A:08X}u, sub_{g.A:08X}); }}" in c, c
    assert f"sub_{g.A:08X}_gen" not in c, c


def test_wrapped_conditional_tail_names_the_wrapper():
    compare = Instruction(0x00120000, 2, "cmp", "eax, ecx", "39c8")
    compare.operands = [
        Operand(type="reg", reg="eax"),
        Operand(type="reg", reg="ecx"),
    ]
    jump = Instruction(
        0x00120002, 6, "je", f"0x{TARGET:x}", "0f8400000000")
    jump.jump_target = TARGET
    lifter = Lifter(
        func_db={TARGET: {"name": "sub_001E9100_gen"}}, wrapped_functions={TARGET})
    lifter.func_start = 0x00120000
    lifter.func_end = 0x00120100

    generated, _ = lift_basic_block(
        lifter, BasicBlock(start=0x00120000, instructions=[compare, jump]))
    output = "\n".join(generated)

    assert "sub_001E9100(); return; }" in output
    assert "sub_001E9100_gen" not in output
