#!/usr/bin/env python3
"""Makes the ps2_import fixtures: a made-up Burnout 3 PS2 save (no player's
data) in every container the importer reads.

    python3 make_fixtures.py [--card-tool <python that has mymcplus>]

a.dat  the PS2 data file: one profile in use (TESTER), two empty
b.dat  two profiles in use (TESTER in 1, SECOND in 3)
a.psu, a.max, a.cbs, a.xps, a.psv   a.dat in each single-save format
b.psu
card-ecc.ps2.deflate, card-plain.ps2.deflate   an 8 MB memory card holding
    a.psu, with the ECC bytes and without, raw-deflated

The .psu, .max (with an LZARI packer of its own), .cbs, .xps and .psv are
written here. The memory card is made by mymcplus (format, import), run as
a program and only when asked: its output is data, none of its code is used
by the importer.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fixtures")
DIR = "BASLUS-21050"

DATA = 0xF14C
PROF0 = 0xE374
PLEN = 0x49C


# ── the save ─────────────────────────────────────────────────────────

def profile(name, used, seed):
    p = bytearray(PLEN)
    if not used:
        p[8:8 + 2 * len("BURNOUT")] = "BURNOUT".encode("utf-16-le")
        for k in range(173):
            p[0x386 + k] = 0xFF
        p[0x494:0x49C] = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x01])
        return p
    struct.pack_into("<If", p, 0, 0x0A1B8409 + seed, 3600.0 * (10 + seed))
    p[8:8 + 2 * len(name)] = name.encode("utf-16-le")
    struct.pack_into("<III", p, 0x3C, 0, 123456 + seed, 7654321 + seed)
    for k in range(0x154, 0x384, 4):
        struct.pack_into("<I", p, k, (k * 7 + seed) & 0xFFFF)
    for k in range(173):
        p[0x386 + k] = (3, 2, 1, 0xFF)[(k + seed) % 4]
    for k in range(20):
        p[0x433 + k] = (k + seed) & 1
    for k in range(10):
        p[0x447 + k] = 1
    p[0x494] = 40
    p[0x495] = 60
    p[0x496:0x49C] = bytes([3, 3, 0xFF, 1, 0, 0])      # last byte 0: in use
    return p


def save(slots):
    d = bytearray(DATA)
    struct.pack_into("<I", d, 0, 0x10)
    # The records table: 28-byte records (a name, a car, a value).
    off, i = 4, 0
    while off + 28 <= 0xE334:
        nm = ("REC%d" % (i % 97)).encode("utf-16-le")
        d[off:off + len(nm)] = nm
        struct.pack_into("<HHHf", d, off + 0x12, i % 67, 0x9A00 | (i & 0xFF), 0, 60.0 + (i % 50))
        off += 28
        i += 1
    set_ = bytes([0x07, 0x0F] * 22) + bytes([1, 1, 1, 1, 1, 0xFF, 0, 0, 0x5A, 0x5A, 0x5A, 0x12, 0, 0, 1, 1, 0, 1, 1, 1])
    d[0xE334:0xE374] = set_
    for s, p in enumerate(slots):
        d[PROF0 + s * PLEN:PROF0 + (s + 1) * PLEN] = p
    total = sum(struct.unpack("<%dI" % ((DATA - 4) // 4), bytes(d[:DATA - 4]))) & 0xFFFFFFFF
    struct.pack_into("<I", d, DATA - 4, total)
    return bytes(d)


ICON_SYS = b"PS2D" + bytes(960)
FILES = lambda data: [("icon.sys", ICON_SYS), (DIR, data)]
TOD = bytes([0, 1, 2, 3, 4, 5]) + struct.pack("<H", 2004)       # (x, sec, min, hour, day, month, year)


# ── containers ───────────────────────────────────────────────────────

def dirent(mode, length, name, cluster=0):
    e = bytearray(512)
    struct.pack_into("<HHI8sII8sI", e, 0, mode, 0, length, TOD, cluster, 0, TOD, 0)
    e[0x40:0x40 + len(name)] = name.encode("ascii")
    return bytes(e)


def psu(files):
    out = dirent(0x8427, len(files) + 2, DIR) + dirent(0x8427, 0, ".") + dirent(0xA426, 0, "..")
    for name, data in files:
        out += dirent(0x8497, len(data), name) + data + bytes((-len(data)) % 1024)
    return out


class LzariEncoder:
    """Okumura's LZARI, greedy matches from a small hash of 3-byte runs."""
    N, F, TH = 4096, 60, 2
    NCHAR = 256 - 2 + 60
    Q1 = 1 << 15
    Q2, Q3, Q4 = 2 * Q1, 3 * Q1, 4 * Q1

    def __init__(self):
        n = self.NCHAR
        self.char_to_sym = [0] * n
        self.sym_to_char = [0] * (n + 1)
        self.sym_freq = [0] * (n + 1)
        self.sym_cum = [0] * (n + 1)
        for sym in range(n, 0, -1):
            ch = sym - 1
            self.char_to_sym[ch] = sym
            self.sym_to_char[sym] = ch
            self.sym_freq[sym] = 1
            self.sym_cum[sym - 1] = self.sym_cum[sym] + 1
        self.pos_cum = [0] * (self.N + 1)
        for i in range(self.N, 0, -1):
            self.pos_cum[i - 1] = self.pos_cum[i] + 10000 // (i + 200)
        self.low, self.high, self.shifts = 0, self.Q4, 0
        self.bits = []

    def put(self, bit):
        self.bits.append(bit)
        while self.shifts:
            self.bits.append(1 - bit)
            self.shifts -= 1

    def norm(self):
        while True:
            if self.high <= self.Q2:
                self.put(0)
            elif self.low >= self.Q2:
                self.put(1)
                self.low -= self.Q2
                self.high -= self.Q2
            elif self.low >= self.Q1 and self.high <= self.Q3:
                self.shifts += 1
                self.low -= self.Q1
                self.high -= self.Q1
            else:
                break
            self.low += self.low
            self.high += self.high

    def update(self, sym):
        f, c = self.sym_freq, self.sym_cum
        if c[0] >= self.Q1 - 1:
            t = 0
            for i in range(self.NCHAR, 0, -1):
                c[i] = t
                f[i] = (f[i] + 1) >> 1
                t += f[i]
            c[0] = t
        i = sym
        while f[i] == f[i - 1]:
            i -= 1
        if i < sym:
            ci, cs = self.sym_to_char[i], self.sym_to_char[sym]
            self.sym_to_char[i], self.sym_to_char[sym] = cs, ci
            self.char_to_sym[ci], self.char_to_sym[cs] = sym, i
        f[i] += 1
        while i > 0:
            i -= 1
            c[i] += 1

    def char(self, ch):
        sym = self.char_to_sym[ch]
        r = self.high - self.low
        self.high = self.low + r * self.sym_cum[sym - 1] // self.sym_cum[0]
        self.low += r * self.sym_cum[sym] // self.sym_cum[0]
        self.norm()
        self.update(sym)

    def position(self, p):
        r = self.high - self.low
        self.high = self.low + r * self.pos_cum[p] // self.pos_cum[0]
        self.low += r * self.pos_cum[p + 1] // self.pos_cum[0]
        self.norm()

    def encode(self, src):
        heads = {}
        o = 0
        while o < len(src):
            best_len, best_at = 0, 0
            if o + 3 <= len(src):
                for m in reversed(heads.get(src[o:o + 3], [])[-8:]):
                    if o - m > self.N - self.F:
                        continue
                    k = 0
                    while k < self.F and o + k < len(src) and src[m + k] == src[o + k]:
                        k += 1
                    if k > best_len:
                        best_len, best_at = k, m
            step = best_len if best_len > self.TH else 1
            if step > 1:
                self.char(255 - self.TH + best_len)
                self.position((o - best_at - 1) & (self.N - 1))
            else:
                self.char(src[o])
            for q in range(o, o + step):
                if q + 3 <= len(src):
                    heads.setdefault(src[q:q + 3], []).append(q)
            o += step
        self.shifts += 1
        self.put(0 if self.low < self.Q1 else 1)
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def max_file(files):
    body = b""
    for name, data in files:
        body += struct.pack("<I32s", len(data), name.encode("ascii")) + data
        body += bytes((-(len(body) + 8)) % 16)
    packed = LzariEncoder().encode(body)
    hdr = struct.pack("<12sI32s32sIII", b"Ps2PowerSave", 0, DIR.encode(), b"Burnout 3", len(packed) + 4,
                      len(files), len(body))
    crc = zlib.crc32(hdr + packed) & 0xFFFFFFFF
    return hdr[:12] + struct.pack("<I", crc) + hdr[16:] + packed


def cbs_state():
    """The CodeBreaker key state, read from the importer's own table."""
    import re
    src = open(os.path.join(HERE, "..", "..", "src", "kernel", "kernel_ps2save.c")).read()
    body = src[src.index("k_cbs_state[256] = {"):]
    body = body[:body.index("};")]
    vals = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", body)]
    assert len(vals) == 256 and sorted(vals) == list(range(256))
    return bytes(vals)


def rc4(data):
    s = bytearray(cbs_state())
    out = bytearray(len(data))
    j = 0
    for k in range(len(data)):
        i = (k + 1) & 255
        j = (j + s[i]) & 255
        s[i], s[j] = s[j], s[i]
        out[k] = data[k] ^ s[(s[i] + s[j]) & 255]
    return bytes(out)


def cbs(files):
    body = b""
    for name, data in files:
        body += struct.pack("<8s8sIHHII32s", TOD, TOD, len(data), 0x8497, 0, 0, 0, name.encode()) + data
    packed = rc4(zlib.compress(body, 9))
    hlen = 0x128
    hdr = struct.pack("<4sIIII32s8s8sIIIIII", b"CFU\0", 0x1F00, hlen, len(body), len(packed), DIR.encode(),
                      TOD, TOD, 0, 0, 0x8427, 0, 0, 0)
    hdr += b"Burnout 3".ljust(hlen - len(hdr), b"\0")
    return hdr + packed


def sharkport(files):
    def s(b):
        return struct.pack("<I", len(b)) + b

    def hdr(name, size, mode):
        swapped = ((mode & 0xFF) << 8) | (mode >> 8)
        return struct.pack("<H64sI8xH2x8s8s", 98, name.encode(), size, swapped, TOD, TOD)

    rest = hdr(DIR, len(files) + 2, 0x8427)
    for name, data in files:
        rest += hdr(name, len(data), 0x8497) + data
    out = b"\x0d\0\0\0SharkPortSave" + struct.pack("<I", 0) + s(DIR.encode()) + s(b"2004-10-09") + s(b"made up")
    return out + struct.pack("<I", len(rest)) + rest + bytes(4)


def psv(files):
    count = len(files)
    out = bytearray(b"\0VSP" + struct.pack("<I", 0) + bytes(40) + bytes(8) + struct.pack("<II", 0x2C, 2))
    data_at = 0xA0 + 60 * count
    out += struct.pack("<10I", 0, 0, 0, 0, 0, 0, 0, 0, 0, count)
    out += struct.pack("<8s8sII32s", TOD, TOD, 0, 0x8427, DIR.encode())
    blobs = b""
    for name, data in files:
        out += struct.pack("<8s8sII32sI", TOD, TOD, len(data), 0x8497, name.encode(), data_at + len(blobs))
        blobs += data
    assert len(out) == 0xA0 + 60 * count
    return bytes(out) + blobs


def card(psu_path, tool):
    with tempfile.TemporaryDirectory() as t:
        c = os.path.join(t, "card.ps2")
        subprocess.run([tool, "-I", "-m", "mymcplus", c, "format"], check=True)
        subprocess.run([tool, "-I", "-m", "mymcplus", c, "import", psu_path], check=True)
        with open(c, "rb") as f:
            ecc = f.read()
    assert len(ecc) == 8650752
    plain = b"".join(ecc[i:i + 512] for i in range(0, len(ecc), 528))
    for name, b in (("card-ecc.ps2.deflate", ecc), ("card-plain.ps2.deflate", plain)):
        z = zlib.compressobj(9, zlib.DEFLATED, -15)
        put(name, z.compress(b) + z.flush())


def put(name, b):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(b)


def main():
    os.makedirs(OUT, exist_ok=True)
    a = save([profile("TESTER", True, 0), profile("", False, 1), profile("", False, 2)])
    b = save([profile("TESTER", True, 0), profile("", False, 1), profile("SECOND", True, 5)])
    put("a.dat", a)
    put("b.dat", b)
    put("a.psu", psu(FILES(a)))
    put("b.psu", psu(FILES(b)))
    put("a.max", max_file(FILES(a)))
    put("a.cbs", cbs(FILES(a)))
    put("a.xps", sharkport(FILES(a)))
    put("a.psv", psv(FILES(a)))
    if "--card-tool" in sys.argv:
        card(os.path.join(OUT, "a.psu"), sys.argv[sys.argv.index("--card-tool") + 1])
    print("fixtures in", OUT)


if __name__ == "__main__":
    main()
