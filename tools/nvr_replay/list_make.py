#!/usr/bin/env python3
"""A shader list that can ship: nvr_pipelines.bin files (which hold the game's microcode and stay on
your own machines) turned into offsets into one exact XBE, plus hashes and pipeline state.

    list_make.py OUT.nvrptr XBE LIST [LIST...]     merges the lists, writes OUT, then checks it
    list_make.py --check OUT.nvrptr XBE LIST [LIST...]

The check fails, and OUT must not be published, if two consecutive words of any listed shader appear
in OUT. A word not in the XBE drops its shader, which then just builds during play. Format:
src/nvr/nvr_backend.c, "a shipped list"."""
import struct, sys

ZERO = 0xFFFFFFFF

def fnv64(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h

def read_list(path):
    raw = open(path, 'rb').read()
    at = raw.find(b'NvrPsoDisk')
    if at < 0:
        sys.exit(f'{path}: not a native pipeline list')
    at = raw.find(b'\0', at)
    for start in range(at, min(at + 64, len(raw))):
        i, got = start, []
        while i + 16 <= len(raw):
            t, n, c = struct.unpack_from('<IIQ', raw, i)
            if t > 8 or n > 65536 or i + 16 + n > len(raw):
                break
            got.append((t, raw[i + 16:i + 16 + n]))
            i += 16 + n
        if i == len(raw) and got:
            return got
    sys.exit(f'{path}: no records')

def merged(paths):
    vs, fs, pso = {}, {}, {}
    for p in paths:
        for t, d in read_list(p):
            (vs if t == 0 else fs if t == 1 else pso if t == 2 else {})[d] = 1
    return list(vs), list(fs), list(pso)

def vs_hash(data):
    # nv2a_vsh_program_hash, per word.
    h = 2166136261
    for k in range(0, len(data), 4):
        h = ((h ^ int.from_bytes(data[k:k + 4], 'little')) * 16777619) & 0xffffffff
    return h

def fs_hash(data):
    # src/nvr/nvr_backend.c's fnv32, per byte.
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h

def check(out, vs, fs):
    """Two consecutive words of a shader in the list would be game code."""
    bad = 0
    for p in vs:
        for k in range(0, len(p) - 4, 4):
            w = p[k:k + 8]
            if w[:4] != b'\0\0\0\0' and w[4:] != b'\0\0\0\0' and out.find(w) >= 0:
                bad += 1
    for p in fs:
        for k in range(0, 34 * 4, 4):
            w = p[k:k + 8]
            if w[:4] != b'\0\0\0\0' and w[4:] != b'\0\0\0\0' and out.find(w) >= 0:
                bad += 1
    return bad

def main():
    args = sys.argv[1:]
    only_check = args and args[0] == '--check'
    if only_check:
        args = args[1:]
    if len(args) < 3:
        sys.exit(__doc__)
    out_path, xbe_path, lists = args[0], args[1], args[2:]
    vs, fs, pso = merged(lists)
    if only_check:
        n = check(open(out_path, 'rb').read(), vs, fs)
        print(f'{out_path}: {n} pieces of game code found')
        sys.exit(1 if n else 0)
    xbe = open(xbe_path, 'rb').read()
    where = {}
    def offset(w):
        if w == b'\0\0\0\0':
            return ZERO
        if w not in where:
            where[w] = xbe.find(w)
        return where[w]
    out_vs, out_fs, keep_vs, keep_fs = [], [], set(), set()
    drop_vs = drop_fs = 0
    for p in vs:
        offs = [offset(p[k:k + 4]) for k in range(0, len(p), 4)]
        if -1 in offs:
            drop_vs += 1
            continue
        out_vs.append(struct.pack('<IQ', len(p) // 16, fnv64(p)) + struct.pack(f'<{len(offs)}I', *offs))
        keep_vs.add((vs_hash(p), len(p) // 16))
    for p in fs:
        regs = p[:35 * 4]
        offs = [offset(regs[k:k + 4]) for k in range(0, len(regs), 4)]
        if -1 in offs:
            drop_fs += 1
            continue
        out_fs.append(struct.pack('<Q', fnv64(regs)) + struct.pack('<35I', *offs) + p[35 * 4:41 * 4])
        keep_fs.add(fs_hash(p[:41 * 4]))
    out_pso = []
    drop_pso = 0
    for p in pso:
        vh, vl, fh = struct.unpack_from('<III', p)
        if (vl and (vh, vl) not in keep_vs) or (fh and fh not in keep_fs):
            drop_pso += 1
            continue
        out_pso.append(p)
    body = struct.pack('<8sIQIII', b'NVRPTR1\0', len(xbe), fnv64(xbe), len(out_vs), len(out_fs), len(out_pso))
    data = body + b''.join(out_vs) + b''.join(out_fs) + b''.join(out_pso)
    n = check(data, vs, fs)
    print(f'{len(out_vs)} vertex (dropped {drop_vs}), {len(out_fs)} fragment (dropped {drop_fs}), '
          f'{len(out_pso)} pipelines (dropped {drop_pso}), {len(data)} bytes; '
          f'{n} pieces of game code found')
    if n:
        sys.exit(f'{out_path}: not written, it holds game code')
    open(out_path, 'wb').write(data)

main()
