"""Comparison snapshots must survive a join of different CMP operands."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand

BASE = 0x10000

def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_different_cmp_operands_join_for_setne():
    code = translate_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code

def test_different_cmp_operands_join_for_cmovne():
    code = translate_join(bytes.fromhex('0f45c7c3'))
    assert 'if (CMP_NE(_fa, _fb)) eax = edi;' in code, code

def test_unknown_path_is_not_guessed():
    assert _merge_flag_states([None, ('cmp', [])]) is None

def test_mixed_operations_are_not_guessed():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('cmp', [a, b]), ('test', [a, b])]) is None

def test_mixed_widths_merge_but_not_for_sign_or_overflow():
    from tools.recomp.lifter import _make_condition
    wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
    merged = _merge_flag_states([('cmp', wide), ('cmp', narrow)])
    assert merged[0] == 'cmp_mixed'
    assert _make_condition('jne', *merged) is not None
    assert _make_condition('js', *merged) is None

def translate_float_join():
    # xorps xmm2,xmm2; comiss xmm0,xmm2; jbe alt; comiss xmm2,xmm1; jmp join;
    # alt: comiss xmm1,xmm2; join: jbe out; movaps xmm0,xmm2; ret;
    # out: xorps xmm0,xmm0; ret -- the shape of Burnout 3's stick dead zone.
    image = bytes.fromhex('0f57d2' '0f2fc2' '7605' '0f2fd1' 'eb03'
                          '0f2fca' '7604' '0f28c2' 'c3' '0f57c0' 'c3')
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_float_compares_with_different_operands_join():
    code = translate_float_join()
    assert '_flags /* jbe' not in code, code
    # Both jbe forms read the snapshot, the one after the join included.
    assert code.count('(_fca <= _fcb ||') == 2, code

def test_float_compare_joins_only_float_compares():
    x = [Operand(type='reg', reg='xmm0'), Operand(type='reg', reg='xmm1')]
    y = [Operand(type='reg', reg='xmm2'), Operand(type='reg', reg='xmm3')]
    a = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    assert _merge_flag_states([('comiss', x), ('ucomiss', y)]) == ('comiss', x)
    assert _merge_flag_states([('comiss', x), ('cmp', a)]) is None
