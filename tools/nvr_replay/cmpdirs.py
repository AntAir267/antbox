#!/usr/bin/env python3
"""Compare two folders of frame dumps (BMP), by matching names or by flip.

  cmpdirs.py A B [--map 'fNNN.bmp=f%06d.bmp:mul:add']
  cmpdirs.py A B --logs A_LOG B_LOG

With --logs, each run's log ("[GPU] flip N: frame dump", one per file,
in order) pairs every dump with its flip, and the
dumps are compared by flip: RECOMP_FB_DUMP_FLIPS numbers files by count
and skips a flip that drew nothing, so the same name can be another flip.

Per frame: the largest channel difference, the share of pixels off by more
than 8 in any channel, and PSNR. Pure Python (no PIL needed). Exit 0.
"""
import math, os, re, struct, sys

def load(p):
    b = open(p, 'rb').read()
    off, = struct.unpack_from('<I', b, 10)
    w, h = struct.unpack_from('<ii', b, 18)
    bpp, = struct.unpack_from('<H', b, 28)
    n = bpp // 8
    row = (w * n + 3) & ~3
    out = bytearray()
    for y in range(abs(h)):
        yy = (abs(h) - 1 - y) if h > 0 else y
        r = b[off + yy * row: off + yy * row + w * n]
        for x in range(w):
            out += r[x * n:x * n + 3]
    return w, abs(h), out

def cmp(a, b):
    wa, ha, A = load(a)
    wb, hb, B = load(b)
    if (wa, ha) != (wb, hb):
        return 'size %dx%d vs %dx%d' % (wa, ha, wb, hb)
    d = [abs(x - y) for x, y in zip(A, B)]
    n = len(d) // 3
    bad = sum(1 for i in range(n) if max(d[3 * i:3 * i + 3]) > 8)
    mse = sum(v * v for v in d) / len(d)
    psnr = 'inf' if mse == 0 else '%.2f' % (10 * math.log10(65025 / mse))
    return 'max %3d  bad %7.3f%%  PSNR %s' % (max(d), 100.0 * bad / n, psnr)

def by_flip(log):
    """flip -> file of each frame dump. A log that names every file ("[GPU]
    dump .../fNNN.bmp: frame at flip N") says it outright; an older one is
    counted: files are fNNN.bmp in order, one per "[GPU] flip N: frame dump"
    and one per mid-frame "[VSH] 3D batch ...: dumping" (same sequence)."""
    named, m, n = {}, {}, 0
    for line in open(log, errors='replace'):
        r = re.search(r'\[GPU\] dump (\S+\.bmp): frame at flip (\d+)', line)
        if r:
            named[int(r.group(2))] = os.path.basename(r.group(1))
            continue
        r = re.search(r'\[GPU\] flip (\d+): frame dump', line)
        if r:
            m[int(r.group(1))] = 'f%03d.bmp' % n
            n += 1
        elif re.search(r'\[VSH\] 3D batch \d+ .*: dumping', line):
            n += 1
    return named or m

a, b = sys.argv[1], sys.argv[2]
if len(sys.argv) > 5 and sys.argv[3] == '--logs':
    fa, fb = by_flip(sys.argv[4]), by_flip(sys.argv[5])
    both = sorted(set(fa) & set(fb))
    print('dumps: %d and %d, %d at the same flips' % (len(fa), len(fb), len(both)))
    for fl in both:
        print('flip %5d %-10s vs %-10s %s' % (fl, fa[fl], fb[fl],
              cmp(os.path.join(a, fa[fl]), os.path.join(b, fb[fl]))))
    sys.exit(0)
mp = None
if len(sys.argv) > 4 and sys.argv[3] == '--map':
    m = re.match(r'(.*)=(.*):(\d+):(\d+)$', sys.argv[4])
    mp = (m.group(2), int(m.group(3)), int(m.group(4)))
for name in sorted(os.listdir(a)):
    if not name.endswith('.bmp'):
        continue
    other = name
    if mp:
        k = int(re.sub(r'\D', '', name))
        other = mp[0] % (k * mp[1] + mp[2])
    pb = os.path.join(b, other)
    if os.path.exists(pb):
        print('%-12s vs %-14s %s' % (name, other, cmp(os.path.join(a, name), pb)))
    else:
        print('%-12s (no %s)' % (name, other))
