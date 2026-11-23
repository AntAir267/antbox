"""--mem-order: listed lock-release stores become store-releases, and every
spin-wait candidate's exit gets an acquire fence (mem_order.py)."""
import pytest

from . import config
from .mem_order import MemOrderConfig, MemOrderReport
from .translator import FunctionTranslator


def _translate(hexs, mem_order, base=0x10000):
    image = bytes.fromhex(hexs)
    config._install([config.Section(".text", base, len(image), 0, len(image),
                                    True)],
                    entry_point=base, kernel_thunk_addr=base, origin="mem-order")
    db = {base: {"start": hex(base), "end": base + len(image), "_addr": base,
                 "size": len(image)}}
    t = FunctionTranslator(image, db)
    if mem_order is not None:
        t.lifter.mem_order = MemOrderReport(mem_order)
    return t.translate_function(base, db[base]), t.lifter.mem_order


# mov dword ptr [ecx + 8], 0 ; mov byte ptr [ecx], 0 ; ret
_RELEASE = "c7410800000000" + "c60100" + "c3"
# L: cmp dword ptr [ecx], 0 ; jne L ; mov eax, [ecx + 4] ; ret
_SPIN = "833900" + "75fb" + "8b4104" + "c3"


def test_listed_store_is_a_release_store_and_the_others_stay_plain():
    cfg = MemOrderConfig(release_stores={0x10000})
    c, report = _translate(_RELEASE, cfg)
    assert "RECOMP_STORE_REL32(ecx + 8, 0);" in c
    assert "MEM8(ecx) = 0;" in c
    assert report.applied[0x10000] == 1
    assert report.finish() == []
    assert report.to_json()["release_stores"] == [{"va": "0x00010000", "bodies": 1}]


def test_the_narrow_store_takes_its_width():
    cfg = MemOrderConfig(release_stores={0x10007})
    c, _ = _translate(_RELEASE, cfg)
    assert "RECOMP_STORE_REL8(ecx, 0);" in c
    assert "MEM32(ecx + 8) = 0;" in c


def test_without_the_option_the_output_is_unchanged():
    c, report = _translate(_RELEASE, None)
    assert report is None
    assert "RECOMP_STORE_REL" not in c
    c, _ = _translate(_SPIN, None)
    assert "RECOMP_ACQUIRE_FENCE" not in c


def test_a_listed_site_that_is_not_a_store_fails():
    cfg = MemOrderConfig(release_stores={0x1000A})     # the ret
    _, report = _translate(_RELEASE, cfg)
    assert any("0x0001000A" in e and "not a mov to memory" in e
               for e in report.errors)
    cfg = MemOrderConfig(release_stores={0x20000})     # nowhere
    _, report = _translate(_RELEASE, cfg)
    assert any("0x00020000" in e and "no lifted body" in e
               for e in report.finish())


def test_spin_wait_exit_gets_an_acquire_fence_after_the_back_edge():
    cfg = MemOrderConfig(acquire_spin_exits=True)
    c, report = _translate(_SPIN, cfg)
    lines = [l.strip() for l in c.splitlines()]
    back = next(i for i, l in enumerate(lines)
                if l.startswith("if (") and "goto loc_00010000;" in l)
    assert lines[back + 1].startswith("RECOMP_ACQUIRE_FENCE();")
    assert "MEM32(ecx + 4)" in c[c.index("RECOMP_ACQUIRE_FENCE"):]
    assert report.spin_exits == {0x10000: 1}
    assert report.to_json()["spin_exits_fenced"] == [{"va": "0x00010000", "bodies": 1}]


def test_spin_wait_fence_is_independent_of_lowering():
    from .spin_waits import SpinWaitConfig, SpinWaitReport
    cfg = MemOrderConfig(acquire_spin_exits=True)
    image = bytes.fromhex(_SPIN)
    base = 0x10000
    config._install([config.Section(".text", base, len(image), 0, len(image),
                                    True)],
                    entry_point=base, kernel_thunk_addr=base, origin="mem-order")
    db = {base: {"start": hex(base), "end": base + len(image), "_addr": base,
                 "size": len(image)}}
    t = FunctionTranslator(image, db)
    t.lifter.mem_order = MemOrderReport(cfg)
    t.spin_waits = SpinWaitReport(SpinWaitConfig(sites={base}))
    c = t.translate_function(base, db[base])
    assert "RECOMP_SPIN_WAIT(0x00010000u);" in c
    assert c.index("RECOMP_SPIN_WAIT") < c.index("RECOMP_ACQUIRE_FENCE")


def test_a_loop_that_is_not_a_spin_wait_gets_no_fence():
    # L: inc eax ; cmp eax, [ecx] ; jne L ; ret  -- writes a register the
    # matcher refuses (not only loads and compares).
    cfg = MemOrderConfig(acquire_spin_exits=True)
    c, report = _translate("40" + "3b01" + "75fb" + "c3", cfg)
    assert "RECOMP_ACQUIRE_FENCE" not in c
    assert report.spin_exits == {}


def test_config_file(tmp_path):
    p = tmp_path / "mem_order.json"
    p.write_text('{"about": "x", "release_stores": [{"va": "0x0021C671", '
                 '"note": "n"}, "0x10"], "acquire_spin_exits": true}')
    cfg = MemOrderConfig.load(str(p))
    assert cfg.release_stores == {0x21C671, 0x10}
    assert cfg.acquire_spin_exits is True
    assert cfg.notes == {0x21C671: "n"}
    p.write_text('{"sites": []}')
    with pytest.raises(ValueError):
        MemOrderConfig.load(str(p))
