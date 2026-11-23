"""
Guest memory ordering on arm64 hosts (opt-in: --mem-order).

The lifter emits every plain guest load and store as a volatile access
(MEM8/16/32 in recomp_types.h). On an x86-64 host that carries the x86
ordering the title was written against: a store is visible in program order
with the stores before it, a load is not satisfied before the loads after
it. On arm64 (Apple silicon, Android) a plain `str` may become visible
before the stores that preceded it, and the loads after a flag-polling loop
may be satisfied before the load that ended it. A lock released by
`mov [flag], 0` after the owner's writes, or a wait loop that exits on a
flag and then reads the data behind it, is therefore ordered on x86-64 and
not on arm64 (audit 2026-11-03, 1-lifter-atomics F1; tests/mem_order shows
the reordering on Apple silicon).

The fix is per site, not a global rule, and the file says which:

    {"release_stores": [{"va": "0x0021C671", "note": "..."}],
     "acquire_spin_exits": true}

- release_stores: a `mov` to memory at each listed address is emitted as
  RECOMP_STORE_REL{8,16,32} (a store-release: `stlr` on arm64, a plain
  store on x86-64, where every store already is one). A listed address
  that is not a mov to memory, or that no lifted body contains, fails the
  run: a site that no longer matches is never silently skipped.
- acquire_spin_exits: every busy-wait loop the spin-wait matcher
  recognises (spin_waits.match -- cmp/test of memory, loads, and a
  conditional back edge to the block start), lowered or not, gets
  RECOMP_ACQUIRE_FENCE() on its exit, so the loads after the loop are
  ordered after the one that saw the flag. No instruction on x86-64.

Why per site and not every guest access: a global mapping (ldar/stlr for
all guest memory) is wrong before it is slow. arm64's acquire and release
forms fault on an unaligned address, and the guest does unaligned dword
and qword accesses (packet parsing, packed structures, doubles at 4-byte
alignment), so each access would need an alignment branch and a fenced
fallback, on every one of the millions of loads and stores a frame does.
The sharing the title actually relies on is otherwise ordered already:
critical sections, events and the IRQL gate are host primitives with
acquire/release of their own, and xchg/xadd/cmpxchg lift to SEQ_CST
atomics. What is left is exactly the plain-store release and the
plain-load poll, which are what the list and the fence cover.

The report (mem_order.json in -o) counts the bodies each site was applied
in and the spin exits fenced.
"""

import json

from .spin_waits import match as spin_match, _parse_va


class MemOrderConfig:
    """The parsed --mem-order file."""

    def __init__(self, release_stores=(), acquire_spin_exits=False,
                 notes=None):
        self.release_stores = set(release_stores)
        self.acquire_spin_exits = bool(acquire_spin_exits)
        self.notes = dict(notes or {})

    @classmethod
    def load(cls, path):
        with open(path) as f:
            doc = json.load(f)
        if not isinstance(doc, dict):
            raise ValueError(f"{path}: expected a JSON object")
        unknown = set(doc) - {"release_stores", "acquire_spin_exits", "about"}
        if unknown:
            raise ValueError(f"{path}: unknown keys {sorted(unknown)}")
        sites, notes = set(), {}
        for s in doc.get("release_stores", []):
            va = _parse_va(s["va"] if isinstance(s, dict) else s)
            sites.add(va)
            if isinstance(s, dict) and s.get("note"):
                notes[va] = s["note"]
        return cls(sites, doc.get("acquire_spin_exits", False), notes)


_GOTO_RE = "goto loc_{:08X};"


class MemOrderReport:
    """What the run applied, per site, across all bodies."""

    def __init__(self, config):
        self.config = config
        self.release = config.release_stores
        self.applied = {va: 0 for va in self.release}
        self.spin_exits = {}      # va -> bodies fenced
        self.errors = []

    def _err(self, msg):
        if msg not in self.errors:        # a block is lifted more than once
            self.errors.append(msg)

    # -- release stores (called by the lifter) --

    def release_store(self, insn, ops):
        """The width (1, 2, 4) when insn is a listed store, else None. A
        listed address that is not a `mov` to memory is an error."""
        if insn.address not in self.release:
            return None
        if insn.mnemonic != "mov" or not ops or ops[0].type != "mem":
            self._err(f"release store 0x{insn.address:08X}: "
                      f"{insn.mnemonic} {getattr(insn, 'op_str', '')}".rstrip()
                      + " is not a mov to memory")
            return None
        width = ops[0].mem_size or 4
        if width not in (1, 2, 4):
            self._err(f"release store 0x{insn.address:08X}: {width}-byte store")
            return None
        self.applied[insn.address] += 1
        return width

    # -- spin exits (called by the translator, after spin-wait lowering) --

    def fence_spin_exits(self, func_addr, bb, stmts):
        """stmts with RECOMP_ACQUIRE_FENCE() after the back edge when bb is a
        spin-wait candidate (lowered or not)."""
        if not self.config.acquire_spin_exits:
            return stmts
        ok, _ = spin_match(bb)
        if not ok:
            return stmts
        goto = _GOTO_RE.format(bb.start)
        for i in range(len(stmts) - 1, -1, -1):
            s = stmts[i]
            if goto in s and s.lstrip().startswith("if ("):
                out = list(stmts)
                out.insert(i + 1, "RECOMP_ACQUIRE_FENCE(); /* spin-wait exit: "
                                  "the loads after the loop come after the "
                                  "one that ended it */")
                self.spin_exits[bb.start] = self.spin_exits.get(bb.start, 0) + 1
                return out
        self._err(f"spin-wait exit 0x{bb.start:08X} in sub_{func_addr:08X}: "
                  f"no conditional back edge in the lifted block")
        return stmts

    def finish(self):
        for va in sorted(self.release):
            if not self.applied[va]:
                self._err(f"release store 0x{va:08X}: no lifted body contains "
                          f"a mov to memory there")
        return self.errors

    def to_json(self):
        sites = []
        for va in sorted(self.release):
            e = {"va": f"0x{va:08X}", "bodies": self.applied[va]}
            if va in self.config.notes:
                e["note"] = self.config.notes[va]
            sites.append(e)
        return {"release_stores": sites,
                "acquire_spin_exits": self.config.acquire_spin_exits,
                "spin_exits_fenced": [{"va": f"0x{va:08X}", "bodies": n}
                                      for va, n in sorted(self.spin_exits.items())],
                "errors": list(self.errors)}
