#!/usr/bin/env python3
"""How much of a pipeline list (nvr_pipelines.bin) can be found again in the title's XBE?
   list_in_xbe.py LIST XBE    Vertex programs by their slots, words as stored; fragment states by
   their combiner registers. Says what a list free of game code could point at."""
import struct, sys, re
raw = open(sys.argv[1], 'rb').read()
xbe = open(sys.argv[2], 'rb').read()
# Header: magic and format, then the version string; records are found by their own check.
def fnv64(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h
at = raw.find(b'NvrPsoDisk')
at = raw.find(b'\0', at)
recs = []
# After the string come records; walk from the first offset that parses.
for start in range(at, min(at + 64, len(raw))):
    i, got = start, []
    while i + 16 <= len(raw):
        t, n, c = struct.unpack_from('<IIQ', raw, i)
        if t > 8 or n > 65536 or i + 16 + n > len(raw):
            break
        got.append((t, raw[i + 16:i + 16 + n]))
        i += 16 + n
    if i == len(raw) and got:
        recs = got
        break
print('records', len(recs), 'xbe', len(xbe))
vs = [p for t, p in recs if t == 0]
fs = [p for t, p in recs if t == 1]
pso = [p for t, p in recs if t == 2]
# As nv2a_pcache_scan_vsh: u16 0x2078, u16 n, then n slots: word 0 is 0, FINAL only on the last.
progs = set(); bodies = []
for m in re.finditer(b'\x78\x20', xbe):
    o = m.start()
    if o + 4 > len(xbe): continue
    n = struct.unpack_from('<H', xbe, o + 2)[0]
    if not 1 <= n <= 136 or o + 4 + n * 16 > len(xbe): continue
    b = xbe[o + 4:o + 4 + n * 16]
    ok = True
    for k in range(n):
        w = struct.unpack_from('<4I', b, k * 16)
        if w[0] != 0 or bool(w[3] & 1) != (k == n - 1):
            ok = False; break
    if ok:
        progs.add(b); bodies.append(b)
print('programs in the executable', len(progs))
def find_vs(p):
    if p in progs: return 'exact'
    # D3D appends a viewport epilogue to a program not marked screenspace: the executable holds
    # the program without it, with FINAL moved.
    for b in progs:
        n = len(b)
        if n < len(p) and p[:n - 16] == b[:n - 16] and p[n - 16:n - 4] == b[n - 16:n - 4]:
            return 'prefix'
    return 'slots-elsewhere' if xbe.find(p[:32]) >= 0 else 'not found'
c = {}
for p in vs:
    k = find_vs(p); c[k] = c.get(k, 0) + 1
print('vertex', len(vs), c)
c = {}
for p in fs:
    # key[41]: rc_cicw 8, aicw 8, cocw 8, aocw 8, ctl, fcw0, fcw1, ...
    key = struct.unpack_from('<41I', p)
    ci = struct.pack('<8I', *key[:8]); ai = struct.pack('<8I', *key[8:16])
    k = 'both' if xbe.find(ci) >= 0 and xbe.find(ai) >= 0 else 'one' if xbe.find(ci) >= 0 or xbe.find(ai) >= 0 else 'no'
    c[k] = c.get(k, 0) + 1
print('fragment', len(fs), c)
print('pipelines', len(pso))
