"""The `_flags` fallback report: each site once, with why its state was lost."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000

# test ecx,ecx; jz alt; cmp eax,0; jmp join; alt: dec eax;
# join: je out; nop; out: ret -- a compare and an arithmetic setter, which no
# merge answers (the follow-up join shape in recomp-test-flag-join D5).
STAGE1_JOIN = bytes.fromhex('85c97405' '83f800' 'eb01' '48'
                            '7401' '90' 'c3')
JOIN_JE = BASE + 2 + 2 + 3 + 2 + 1


def translator_for(image, starts):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='flag-fallback-test')
    end = BASE + len(image)
    db = {s: {'start': hex(s), 'end': end, '_addr': s, 'size': end - s}
          for s in starts}
    return FunctionTranslator(image, db), db


def test_an_unmerged_join_is_reported_with_both_kinds():
    ft, db = translator_for(STAGE1_JOIN, [BASE])
    code = ft.translate_function(BASE, db[BASE])
    assert '_flags /* je' in code, code
    site = ft.flag_fallbacks.sites[(JOIN_JE, 'je')]
    assert site['reason'] == 'join: cmp32z + dec:eax', site
    assert ft.flag_fallbacks.summary()['joins'] == 1


def test_an_alias_body_counts_the_site_once_and_keeps_the_owner_reason():
    ft, db = translator_for(STAGE1_JOIN, [BASE, JOIN_JE])
    ft.translate_function(JOIN_JE, db[JOIN_JE])     # alias: enters at the je
    ft.translate_function(BASE, db[BASE])
    summary = ft.flag_fallbacks.summary()
    assert summary['unique_sites'] == 1, ft.flag_fallbacks.sites
    assert summary['emitted'] == 2
    site = ft.flag_fallbacks.sites[(JOIN_JE, 'je')]
    assert site['bodies'] == 2
    assert site['reason'] == 'join: cmp32z + dec:eax'


def test_an_in_block_clobber_is_reported_as_such():
    # mul ecx; je out; nop; out: ret
    ft, db = translator_for(bytes.fromhex('f7e1' '7401' '90' 'c3'), [BASE])
    ft.translate_function(BASE, db[BASE])
    site = ft.flag_fallbacks.sites[(BASE + 2, 'je')]
    assert site['reason'] == 'in-block undefined after mul'


def test_observed_seeds_name_their_functions():
    ft, db = translator_for(STAGE1_JOIN, [BASE])
    ft.translate_function(BASE, db[BASE])
    assert ft.flag_fallbacks.summary({BASE})['observed_functions'] == [
        ft.flag_fallbacks.sites[(JOIN_JE, 'je')]['function']]
    assert ft.flag_fallbacks.summary(set())['observed_functions'] == []
