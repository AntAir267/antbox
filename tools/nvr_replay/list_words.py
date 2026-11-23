#!/usr/bin/env python3
"""Could a pipeline list point into the player's executable instead of holding code? For each vertex
   program, is every 32-bit word somewhere in the XBE?   list_words.py LIST XBE"""
import struct, sys
raw = open(sys.argv[1], 'rb').read(); xbe = open(sys.argv[2], 'rb').read()
at = raw.find(b'NvrPsoDisk'); at = raw.find(b'\0', at)
for start in range(at, min(at + 64, len(raw))):
    i, got = start, []
    while i + 16 <= len(raw):
        t, n, c = struct.unpack_from('<IIQ', raw, i)
        if t > 8 or n > 65536 or i + 16 + n > len(raw): break
        got.append((t, raw[i + 16:i + 16 + n])); i += 16 + n
    if i == len(raw) and got: break
vs = [p for t, p in got if t == 0]
fs = [p for t, p in got if t == 1]
seen = {}
def has(w):
    if w not in seen: seen[w] = xbe.find(w) >= 0
    return seen[w]
full = 0; missing_words = 0; words = 0; worst = 0; whole = 0
for p in vs:
    whole += xbe.find(p) >= 0
    miss = 0
    for k in range(0, len(p), 4):
        w = p[k:k + 4]
        if w == b'\0\0\0\0': continue
        words += 1
        if not has(w): miss += 1
    missing_words += miss
    full += miss == 0
    worst = max(worst, miss)
print(f'vertex programs {len(vs)}: whole in the file {whole}; every word in the file {full}; '
      f'words {words}, missing {missing_words}, worst program missing {worst}')
fwords = fmiss = ffull = 0
for p in fs:
    key = struct.unpack_from('<35I', p)
    m = 0
    for x in key:
        if x:
            fwords += 1
            if not has(struct.pack('<I', x)): m += 1
    fmiss += m; ffull += m == 0
print(f'fragment states {len(fs)}: every register word in the file {ffull}; words {fwords}, missing {fmiss}')
print('pipelines', sum(1 for t, p in got if t == 2), 'records of state only')
