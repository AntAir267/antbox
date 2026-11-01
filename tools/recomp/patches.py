"""
Opt-in guest patches (--patches): a value a guest instruction loads, replaced
while a setting says so.

A title sometimes carries a behaviour it never switches on for the player's
region: Burnout 3's US build has the km/h speedometer of its other languages,
chosen by the language index it loads at nine places. Rather than rewrite the
function or the data, the recompiler follows the load with a call:

    eax = MEM32(0x45B9BC);
    eax = RECOMP_PATCH_U32("game.speed_units", "kmh", 2u, eax); /* patch 0x... */

RECOMP_PATCH_U32 (templates/runtime/recomp_types.h) returns the value the
instruction loaded unless the setting named by the first argument currently
reads the second, then it returns the third. The runtime (src/platform/
recomp_patch.h) maps the setting key to the environment key the overlay and
the settings file already drive, so the choice is live, and a host that never
enables the setting runs the title as it was. Memory is untouched: the guest
variable keeps its real value for every other reader.

The file (JSON):
    {"patches": [{"va": "0x0005051A", "kind": "load_u32",
                  "setting": "game.speed_units", "when": "kmh", "value": 2,
                  "note": "..."}]}

Optional "latch": "set" or "use" (RECOMP_PATCH_U32_LATCH): the title builds
part of a display once from the choice and reads it again every frame (the
speedometer's unit label and its number). The "set" site samples the setting
and the "use" sites follow that sample, so a change made mid-race shows
whole, at the next set-up, and never as 171 "mph".

kind "load_u32": the instruction at va must be a 32-bit `mov reg, [mem]`.
Its address may sit in several lifted bodies (the lifter splits a function
that another one jumps into); every body gets the call. A listed site that
is not such an instruction, or that no body contains, is an error: a shape
the patcher does not know is never forced through.

kind "load_u8": the same for a byte, `mov r8, [mem]` into al, bl, cl or dl
(a title's own on/off flag, tested right after the load: Burnout 3's "blur
enabled" byte of its graphics manager). The value is 0..255 and only the
register's low byte changes:

    SET_LO8(eax, MEM8(0x4D61D2));
    SET_LO8(eax, RECOMP_PATCH_U32("graphics.motion_blur", "0", 0x0u, LO8(eax))); /* patch 0x... */

kind "cmp_u8": the flag is not loaded but compared where it lies, `cmp byte
ptr [mem], r8|imm8` (Burnout 3's "a crash camera is on" byte, compared with a
zeroed bl and branched on some instructions later). The lifter snapshots a
compare's operands into _fa / _fb, and every later jcc, setcc and cmovcc
reads the snapshot: the call rewrites the left one, so the compare reads
`value` where the memory byte was, whatever sits between it and the branch:

    _fa = (uint32_t)(MEM8(0x73BACA)) & 0xFFu; _fb = (uint32_t)(LO8(ebx)) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp ... */
    _fa = RECOMP_PATCH_U32("graphics.crash_60fps", "1", 0x0u, _fa) & 0xFFu; _fas = (int32_t)(int8_t)(_fa); /* patch 0x... */

(and `_cf = (int)(_fa < _fb);` again in a function that tracks the carry).
No latch.

kind "cmp_u32": the same for a 32-bit value compared in memory, `cmp dword
ptr [mem], r32|imm` (Burnout 3's player count, compared with 1 where its
music manager decides to stay silent in split screen):

    _fa = RECOMP_PATCH_U32("audio.split_music", "1", 0x1u, _fa); _fas = (int32_t)(_fa); /* patch 0x... */

kind "store_u8": a byte the title stores, `mov byte ptr [mem], imm8|r8`, is
stored again as the value while the setting says so (Burnout 3's two-player
stage asks for its half frame rate by storing 1 in a request byte once, at
set-up). Unlike a load patch this writes the guest's memory, as "store_f32"
does: every later reader sees the value.

    MEM8(0x4D538D) = 1;
    MEM8(0x4D538D) = (uint8_t)RECOMP_PATCH_U32("graphics.split_60fps", "1", 0x0u, MEM8(0x4D538D)); /* patch 0x... */

kind "store_u32": the same for a 32-bit value, `mov dword ptr [mem],
imm32|r32` (Burnout 3's stage start stores its simulation stepper's mode, 0,
one step a frame; stored again as 1, the title's own wall-clock mode):

    MEM32(0x4D5390) = 0x0u;
    MEM32(0x4D5390) = RECOMP_PATCH_U32("game.race_clock", "1", 0x1u, MEM32(0x4D5390)); /* patch 0x... */

kind "load_f32" and "store_f32": the value is a float, and it is not a
constant but an expression ("expr" instead of "value"), because a layout
value depends on the picture's shape:

    {"va": "0x00053858", "kind": "load_f32", "setting": "display.hud_fix",
     "when": "1", "expr": "loaded + (640 - 480 * aspect) / 2"}

    xmm0 = XMM_SCALAR(MEMF(0x3A7964)); /* movss */
    xmm0.f[0] = RECOMP_PATCH_F32("display.hud_fix", "1",
        ((double)(xmm0.f[0]) + (640.0 - 480.0 * recomp_patch_var("aspect")) / 2.0),
        xmm0.f[0]);

The expression is arithmetic (+ - * / and parentheses) over numbers,
`loaded` (the float the instruction moved) and names the host gives a value
with recomp_patch_set_var ("aspect": the picture's width over its height).
`f32[esp + 0x30]` reads another guest float at the site (a width squeezed
about an anchor needs the anchor). It is evaluated in double and only while
the setting reads `when`.
"load_f32" takes `movss xmm, [mem]`, the x87 `fld dword ptr [mem]` (the
value pushed, `fp_top()`, is the one replaced; Burnout 3's blur opacity is
read so) and the 32-bit `mov reg, [mem]` (a float moved as bits); "store_f32" takes `movss [mem], xmm` and `mov [mem], reg`
and rewrites the stored float, for a value that shares its register with
other uses.

kind "cmp_f32": a float compared where it lies, `comiss|ucomiss xmm,
dword ptr [mem]` (Burnout 3's rival marker compares the rival's depth with
a distance in its constant pool, 35.0, shared with other code). The lifter
snapshots the compare's operands into _fca / _fcb (doubles) and the jcc
reads them; the call rewrites _fcb, the memory operand, with the
expression (`loaded` is the float the compare read). Memory is untouched:

    _fca = xmm0.f[0]; _fcb = MEMF(0x3B175C); /* comiss */
    _fcb = RECOMP_PATCH_F32("display.rival_marker", "farther", 0, (75.0), _fcb); /* patch 0x... */

kind "mark" changes nothing in the guest: after the instruction at va (any
instruction) the generated code calls RECOMP_PATCH_MARK("name"), and a host
that registered a function for that name (recomp_patch_on_mark) learns the
title got there: "a stage is being set up", "the 3D world was drawn this
frame". It takes "name" instead of a setting; without a registered function
the call does nothing.

    {"va": "0x0001A6A0", "kind": "mark", "name": "stage_render"}

Optional "if": {"reg": "edi", "in": ["0x60EA14", "0x60EA84"]}: the site is
in code several objects share, and the patch is for those whose address the
register holds there. The other calls run as they were. Float patches of one
kind may share a va when each has its own "if" (one value for these objects,
another for those).
"""

import json
import re

_KINDS = frozenset({"load_u32", "load_u8", "cmp_u8", "cmp_u32", "store_u8",
                    "store_u32", "load_f32", "store_f32", "cmp_f32", "mark"})
_INT_KINDS = frozenset({"load_u32", "load_u8", "cmp_u8", "cmp_u32", "store_u8",
                        "store_u32"})
_BYTE_KINDS = frozenset({"load_u8", "cmp_u8", "store_u8"})
_NO_LATCH = frozenset({"cmp_u8", "cmp_u32", "store_u8", "store_u32"})
_REG8 = {"al": "eax", "bl": "ebx", "cl": "ecx", "dl": "edx"}
_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
_XMM = frozenset(f"xmm{i}" for i in range(8))
_TOKEN_RE = re.compile(r"\s*(?:(\d+\.?\d*(?:[eE][+-]?\d+)?|\.\d+)"
                       r"|(f32\[\s*e(?:ax|bx|cx|dx|si|di|bp|sp)\s*"
                       r"(?:[+-]\s*0x[0-9A-Fa-f]{1,4}\s*)?\]"
                       r"|[A-Za-z_][A-Za-z0-9_]*)|([-+*/()]))")
_F32_RE = re.compile(r"f32\[\s*(e..)\s*(?:([+-])\s*0x([0-9A-Fa-f]+)\s*)?\]")


def compile_expr(text):
    """A patch expression as a C expression of type double.

    Grammar: + - * / (unary minus too), parentheses, decimal numbers,
    `loaded`, variable names, and `f32[reg + 0xNN]`: another guest float,
    read as it is at the site (a neighbour of the patched value on the
    guest's stack frame or in its object). Returns (c_text, names); `loaded` stays a
    placeholder, `{loaded}`, for the site to fill in. Anything else is a
    ValueError: the text goes into generated code, so nothing unparsed does.
    """
    toks, pos = [], 0
    text = text.strip()
    while pos < len(text):
        m = _TOKEN_RE.match(text, pos)
        if not m:
            raise ValueError(f"bad expression {text!r} at {text[pos:]!r}")
        toks.append(m.groups())
        pos = m.end()
    names, i = [], [0]

    def peek():
        return toks[i[0]][2] if i[0] < len(toks) else None

    def atom():
        if i[0] >= len(toks):
            raise ValueError(f"expression {text!r} ends early")
        num, name, op = toks[i[0]]
        i[0] += 1
        if num is not None:
            return repr(float(num))
        if name is not None:
            if name == "loaded":
                return "{loaded}"
            m = _F32_RE.fullmatch(name)
            if m:
                reg, sign, disp = m.groups()
                addr = reg if disp is None else f"{reg} {sign} 0x{int(disp, 16):X}"
                return f"(double)MEMF({addr})"
            if name.startswith("f32"):
                raise ValueError(f"expression {text!r}: bad {name!r}")
            if name not in names:
                names.append(name)
            return f'recomp_patch_var("{name}")'
        if op == "-":
            return f"(-{atom()})"
        if op == "(":
            inner = total()
            if peek() != ")":
                raise ValueError(f"expression {text!r}: missing )")
            i[0] += 1
            return f"({inner})"
        raise ValueError(f"expression {text!r}: unexpected {op!r}")

    def product():
        out = atom()
        while peek() in ("*", "/"):
            op = toks[i[0]][2]
            i[0] += 1
            out = f"{out} {op} {atom()}"
        return out

    def total():
        out = product()
        while peek() in ("+", "-"):
            op = toks[i[0]][2]
            i[0] += 1
            out = f"{out} {op} {product()}"
        return out

    out = total()
    if i[0] != len(toks):
        raise ValueError(f"expression {text!r}: trailing text")
    return f"({out})", names
_SETTING_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")
_WHEN_RE = re.compile(r"^!?[A-Za-z0-9_+./:-]+$")
_REG32 = frozenset({"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"})


def _parse_va(v):
    return int(v, 16) if isinstance(v, str) else int(v)


class Patch:
    def __init__(self, va, kind, setting, when, value, note="", latch=None,
                 expr=None, guard=None):
        self.va, self.kind, self.setting = va, kind, setting
        self.when, self.value, self.note = when, value, note
        self.latch = latch
        # Float kinds: the source text, its C form and the names it reads.
        self.expr = expr
        self.c_expr, self.names = compile_expr(expr) if expr else (None, [])
        # "if": (register, [addresses]) or None.
        self.guard = guard


class PatchConfig:
    """The parsed patch file."""

    def __init__(self, patches=()):
        self.patches = {}
        # A site in shared code may carry one patch per set of objects: the
        # further ones at a va, each with its own "if" (as the first has).
        self.more = {}
        for p in patches:
            if p.va in self.patches:
                first = self.patches[p.va]
                if (p.guard is None or first.guard is None
                        or p.kind != first.kind or p.kind in _INT_KINDS):
                    raise ValueError(
                        f"patch 0x{p.va:08X} listed twice (only float "
                        f"patches of one kind, each with an 'if', may share "
                        f"a site)")
                self.more.setdefault(p.va, []).append(p)
                continue
            self.patches[p.va] = p

    @classmethod
    def load(cls, path):
        with open(path) as f:
            doc = json.load(f)
        if not isinstance(doc, dict):
            raise ValueError(f"{path}: expected a JSON object")
        unknown = set(doc) - {"patches", "about"}
        if unknown:
            raise ValueError(f"{path}: unknown keys {sorted(unknown)}")
        out = []
        for s in doc.get("patches", []):
            bad = set(s) - {"va", "kind", "setting", "when", "value", "note",
                            "latch", "expr", "if", "name"}
            if bad:
                raise ValueError(f"{path}: unknown patch keys {sorted(bad)}")
            kind = s.get("kind", "load_u32")
            if kind not in _KINDS:
                raise ValueError(f"{path}: unknown patch kind {kind!r}")
            if kind == "mark":
                if set(s) - {"va", "kind", "name", "note"} or not \
                        _NAME_RE.match(str(s.get("name", ""))):
                    raise ValueError(f"{path}: mark takes 'va' and a 'name'")
                p = Patch(_parse_va(s["va"]), kind, "", "", None,
                          s.get("note", ""))
                p.name = s["name"]
                out.append(p)
                continue
            if "name" in s:
                raise ValueError(f"{path}: only a mark takes 'name'")
            setting, when = s["setting"], s["when"]
            if not _SETTING_RE.match(setting):
                raise ValueError(f"{path}: bad setting key {setting!r}")
            if not _WHEN_RE.match(when):
                raise ValueError(f"{path}: bad 'when' {when!r}")
            value = expr = None
            if kind in _INT_KINDS:
                if "expr" in s:
                    raise ValueError(f"{path}: {kind} takes 'value'")
                value = _parse_va(s["value"])
                top = 0xFF if kind in _BYTE_KINDS else 0xFFFFFFFF
                if not 0 <= value <= top:
                    raise ValueError(
                        f"{path}: value {value} is not a "
                        f"{'uint8' if kind in _BYTE_KINDS else 'uint32'}")
                if kind in _NO_LATCH and "latch" in s:
                    raise ValueError(f"{path}: {kind} takes no latch")
            else:
                if "value" in s or not isinstance(s.get("expr"), str):
                    raise ValueError(f"{path}: {kind} takes 'expr', a string")
                expr = s["expr"]
                compile_expr(expr)
            guard = None
            if "if" in s:
                g = s["if"]
                if (not isinstance(g, dict) or set(g) != {"reg", "in"}
                        or g["reg"] not in _REG32 or not g["in"]
                        or not isinstance(g["in"], list)):
                    raise ValueError(
                        f"{path}: 'if' is {{\"reg\": r32, \"in\": [va, ...]}}")
                guard = (g["reg"], [_parse_va(v) for v in g["in"]])
            latch = s.get("latch")
            if latch not in (None, "set", "use"):
                raise ValueError(f"{path}: latch must be 'set' or 'use'")
            out.append(Patch(_parse_va(s["va"]), kind, setting, when, value,
                             s.get("note", ""), latch, expr, guard))
        return cls(out)


class PatchReport:
    """Which listed sites were patched, per VA, across all bodies."""

    def __init__(self, config):
        self.config = config
        self.applied = {va: 0 for va in config.patches}
        self.errors = []

    def _err(self, msg):
        if msg not in self.errors:       # a block is lifted more than once
            self.errors.append(msg)

    def after(self, insn, func_addr=None, needs_cf=False):
        """Statements to emit after the lifted instruction, or []."""
        p = self.config.patches.get(insn.address)
        if p is None:
            return []
        stmts = []
        for p in [p] + self.config.more.get(insn.address, []):
            if p.kind == "mark":
                stmts.append(f'RECOMP_PATCH_MARK("{p.name}"); '
                             f"/* patch 0x{p.va:08X}, mark */")
                continue
            if p.kind == "load_u32":
                out = self._load_u32(p, insn)
            elif p.kind == "load_u8":
                out = self._load_u8(p, insn)
            elif p.kind in ("cmp_u8", "cmp_u32"):
                out = self._cmp(p, insn, needs_cf)
            elif p.kind == "store_u8":
                out = self._store_u8(p, insn)
            elif p.kind == "store_u32":
                out = self._store_u32(p, insn)
            else:
                out = self._float(p, insn)
            if out is None:
                return []
            if p.guard:
                reg, vas = p.guard
                cond = " || ".join(f"{reg} == 0x{v:X}u" for v in vas)
                out = [f"if ({cond}) {{ {out[0]} }}"]
            stmts.append(f"{out[0]} /* patch 0x{p.va:08X}"
                         + (f", latch {p.latch}" if p.latch else "") + " */")
        self.applied[insn.address] += 1
        return stmts

    def _load_u32(self, p, insn):
        ops = insn.operands
        if (insn.mnemonic != "mov" or len(ops) != 2 or ops[0].type != "reg"
                or ops[0].reg not in _REG32 or ops[1].type != "mem"
                or ops[1].mem_size != 4):
            self._err(
                f"patch 0x{p.va:08X}: {insn.mnemonic} is not a 32-bit "
                f"`mov reg, [mem]` load")
            return None
        reg = ops[0].reg
        if p.latch:
            return [f'{reg} = RECOMP_PATCH_U32_LATCH("{p.setting}", '
                    f'"{p.when}", 0x{p.value:X}u, {reg}, '
                    f"{1 if p.latch == 'set' else 2});"]
        return [f'{reg} = RECOMP_PATCH_U32("{p.setting}", "{p.when}", '
                f"0x{p.value:X}u, {reg});"]

    def _load_u8(self, p, insn):
        """`mov r8, [mem]` into a low byte register: only that byte changes."""
        ops = insn.operands
        if (insn.mnemonic != "mov" or len(ops) != 2 or ops[0].type != "reg"
                or ops[0].reg not in _REG8 or ops[1].type != "mem"
                or ops[1].mem_size != 1):
            self._err(
                f"patch 0x{p.va:08X}: {insn.mnemonic} is not an 8-bit "
                f"`mov al|bl|cl|dl, [mem]` load")
            return None
        reg = _REG8[ops[0].reg]
        if p.latch:
            return [f'SET_LO8({reg}, RECOMP_PATCH_U32_LATCH("{p.setting}", '
                    f'"{p.when}", 0x{p.value:X}u, LO8({reg}), '
                    f"{1 if p.latch == 'set' else 2}));"]
        return [f'SET_LO8({reg}, RECOMP_PATCH_U32("{p.setting}", "{p.when}", '
                f"0x{p.value:X}u, LO8({reg})));"]

    def _cmp(self, p, insn, needs_cf):
        """cmp_u8 / cmp_u32: `cmp byte|dword ptr [mem], reg|imm`: the
        snapshot of the memory operand the lifter took for the compare (_fa,
        and _fas, its sign-extended twin) becomes the value; the carry is
        taken again where it is tracked."""
        ops = insn.operands
        size = 1 if p.kind == "cmp_u8" else 4
        if (insn.mnemonic != "cmp" or len(ops) != 2 or ops[0].type != "mem"
                or ops[0].mem_size != size or ops[1].type not in ("reg", "imm")
                or p.latch):
            self._err(
                f"patch 0x{p.va:08X}: {insn.mnemonic} is not "
                + ("an 8-bit `cmp byte ptr [mem], r8|imm8` compare"
                   if size == 1 else
                   "a 32-bit `cmp dword ptr [mem], r32|imm` compare"))
            return None
        mask, sx = (" & 0xFFu", "(int8_t)") if size == 1 else ("", "")
        return [f'_fa = RECOMP_PATCH_U32("{p.setting}", "{p.when}", '
                f"0x{p.value:X}u, _fa){mask}; "
                f"_fas = (int32_t){sx}(_fa);"
                + (" _cf = (int)(_fa < _fb);" if needs_cf else "")]

    def _store_u8(self, p, insn):
        """`mov byte ptr [mem], imm8|r8`: the stored byte is stored again
        as the value while the setting says so."""
        from .lifter import _fmt_mem
        ops = insn.operands
        if (insn.mnemonic != "mov" or len(ops) != 2 or ops[0].type != "mem"
                or ops[0].mem_size != 1
                or not (ops[1].type == "imm"
                        or (ops[1].type == "reg" and ops[1].reg in _REG8))
                or p.latch):
            self._err(
                f"patch 0x{p.va:08X}: {insn.mnemonic} is not an 8-bit "
                f"`mov byte ptr [mem], imm8|r8` store")
            return None
        where = f"MEM8({_fmt_mem(ops[0])})"
        return [f'{where} = (uint8_t)RECOMP_PATCH_U32("{p.setting}", '
                f'"{p.when}", 0x{p.value:X}u, {where});']

    def _store_u32(self, p, insn):
        """`mov dword ptr [mem], imm32|r32`: the stored value is stored
        again as the value while the setting says so."""
        from .lifter import _fmt_mem
        ops = insn.operands
        if (insn.mnemonic != "mov" or len(ops) != 2 or ops[0].type != "mem"
                or ops[0].mem_size != 4
                or not (ops[1].type == "imm"
                        or (ops[1].type == "reg" and ops[1].reg in _REG32))
                or p.latch):
            self._err(
                f"patch 0x{p.va:08X}: {insn.mnemonic} is not a 32-bit "
                f"`mov dword ptr [mem], imm32|r32` store")
            return None
        where = f"MEM32({_fmt_mem(ops[0])})"
        return [f'{where} = RECOMP_PATCH_U32("{p.setting}", '
                f'"{p.when}", 0x{p.value:X}u, {where});']

    def _float(self, p, insn):
        """load_f32 / store_f32: the moved float, as an lvalue of the right
        type, becomes the expression while the setting says so."""
        from .lifter import _fmt_mem
        ops = insn.operands
        m, ok = insn.mnemonic, len(insn.operands) == 2
        dst, src = (ops[0], ops[1]) if ok else (None, None)
        where = bits = None
        if p.kind == "load_f32" and ok and src.type == "mem" and src.mem_size == 4:
            if m == "movss" and dst.type == "reg" and dst.reg in _XMM:
                where, bits = f"{dst.reg}.f[0]", False
            elif m == "mov" and dst.type == "reg" and dst.reg in _REG32:
                where, bits = dst.reg, True
        elif (p.kind == "load_f32" and len(ops) == 1 and m == "fld"
                and ops[0].type == "mem" and ops[0].mem_size == 4):
            # x87 `fld dword ptr [mem]`: the float just pushed is the top of
            # the stack (a double there).
            where, bits = "fp_top()", False
        elif (p.kind == "cmp_f32" and ok and m in ("comiss", "ucomiss")
                and dst.type == "reg" and dst.reg in _XMM
                and src.type == "mem" and src.mem_size == 4):
            # The compare's snapshot of the memory operand (lifter: comiss).
            where, bits = "_fcb", False
        elif p.kind == "store_f32" and ok and dst.type == "mem" and dst.mem_size == 4:
            if m == "movss" and src.type == "reg" and src.reg in _XMM:
                where, bits = f"MEMF({_fmt_mem(dst)})", False
            elif m == "mov" and src.type == "reg" and src.reg in _REG32:
                where, bits = f"MEM32({_fmt_mem(dst)})", True
        if where is None:
            want = ("`movss xmm, [mem]`, `fld dword ptr [mem]` or 32-bit "
                    "`mov reg, [mem]`"
                    if p.kind == "load_f32" else
                    "`comiss|ucomiss xmm, dword ptr [mem]`"
                    if p.kind == "cmp_f32" else
                    "`movss [mem], xmm` or 32-bit `mov [mem], reg`")
            self._err(f"patch 0x{p.va:08X}: {m} is not a {want}")
            return None
        loaded = f"RECOMP_F32_OF_BITS({where})" if bits else where
        expr = p.c_expr.replace("{loaded}", f"(double)({loaded})")
        macro = "RECOMP_PATCH_F32_BITS" if bits else "RECOMP_PATCH_F32"
        mode = {None: 0, "set": 1, "use": 2}[p.latch]
        return [f'{where} = {macro}("{p.setting}", "{p.when}", {mode}, '
                f"{expr}, {where});"]

    def finish(self):
        """Listed sites no body contained are errors too."""
        for va, n in sorted(self.applied.items()):
            if n == 0:
                self._err(
                    f"patch 0x{va:08X}: no lifted body has an instruction there")
        return self.errors

    def to_json(self):
        return {"sites": [{"va": f"0x{va:08X}", "patched": n > 0,
                           "kind": self.config.patches[va].kind,
                           "setting": self.config.patches[va].setting
                           or getattr(self.config.patches[va], "name", ""),
                           "note": self.config.patches[va].note}
                          for va, n in sorted(self.applied.items())],
                "patched_sites": sum(1 for n in self.applied.values() if n),
                "errors": list(self.errors)}
