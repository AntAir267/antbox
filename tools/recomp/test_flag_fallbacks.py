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


def test_an_unmerged_join_is_evaluated_per_edge_not_reported():
    # A compare on one edge and dec on the other: no merge answers it, and it
    # used to fall back to `_flags` (reported as "join: cmp32z + dec:eax").
    # Each edge now sets the join's variable (upstream 57f47ff6), so there is
    # no fallback to report.
    ft, db = translator_for(STAGE1_JOIN, [BASE])
    code = ft.translate_function(BASE, db[BASE])
    assert '_flags /* je' not in code, code
    assert f'_jf_{JOIN_JE:08X} /* je' in code, code
    assert ft.flag_fallbacks.summary()['unique_sites'] == 0


def test_an_alias_body_reads_its_callers_flags():
    # An alias entering at the je reads flags its caller set (the owner's
    # test or dec, before the jump into it): the caller publishes them to
    # g_eflags and the alias reads them there (upstream fdebc22b), so no
    # fallback is left in either body.
    ft, db = translator_for(STAGE1_JOIN, [BASE, JOIN_JE])
    alias = ft.translate_function(JOIN_JE, db[JOIN_JE])     # enters at the je
    ft.translate_function(BASE, db[BASE])
    assert "(g_eflags & 0x040u) /* popfd */" in alias, alias
    assert ft.flag_fallbacks.summary()['unique_sites'] == 0, ft.flag_fallbacks.sites


def test_an_in_block_clobber_is_reported_as_such():
    # mul ecx; je out; nop; out: ret
    ft, db = translator_for(bytes.fromhex('f7e1' '7401' '90' 'c3'), [BASE])
    ft.translate_function(BASE, db[BASE])
    site = ft.flag_fallbacks.sites[(BASE + 2, 'je')]
    # mul sets CF and OF (in _fa, upstream 47ea2b63) and leaves ZF undefined.
    assert site['reason'] == 'in-block mul cannot answer je'


def test_observed_seeds_name_their_functions():
    # mul ecx; je out: a site that still falls back (the join no longer does).
    ft, db = translator_for(bytes.fromhex('f7e1' '7401' '90' 'c3'), [BASE])
    ft.translate_function(BASE, db[BASE])
    assert ft.flag_fallbacks.summary({BASE})['observed_functions'] == [
        ft.flag_fallbacks.sites[(BASE + 2, 'je')]['function']]
    assert ft.flag_fallbacks.summary(set())['observed_functions'] == []
