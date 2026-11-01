#!/usr/bin/env python3
"""What a capture holds: its spaces, settings (NVRCAP_ENV), HUD squeeze,
and counts of every record type up to its first N kicks.
  capinfo.py CAPTURE [KICKS]"""
import struct, sys
NAMES = {1: 'SPACE', 2: 'PAGE', 3: 'KICK', 4: 'END', 5: 'STATE', 6: 'GAMMA', 7: 'STATE2',
         8: 'SWM', 9: 'ENV', 10: 'HUD'}
f = open(sys.argv[1], 'rb')
assert f.read(8) == b'NVRCAP01'
maxk = int(sys.argv[2]) if len(sys.argv) > 2 else 1 << 30
counts, kicks = {}, 0
while kicks < maxk:
    b = f.read(4)
    if len(b) < 4:
        break
    t, = struct.unpack('<I', b)
    counts[NAMES.get(t, t)] = counts.get(NAMES.get(t, t), 0) + 1
    if t == 1:
        sp, g, sz = struct.unpack('<IIQ', f.read(16)); print('space', sp, hex(g), sz)
    elif t == 2: f.seek(8 + 4096, 1)
    elif t == 3: f.seek(8, 1); kicks += 1
    elif t == 4: break
    elif t == 5: f.seek(12, 1)
    elif t == 6: f.seek(768, 1)
    elif t == 7: f.seek(16, 1)
    elif t == 8: f.seek(4, 1)
    elif t == 9:
        n, = struct.unpack('<I', f.read(4)); print('env', f.read(n).decode(errors='replace'))
    elif t == 10:
        h, k = struct.unpack('<If', f.read(8)); print('hud squeeze', hex(h), k)
    else:
        print('unknown record', t); break
print('records:', counts)
