#!/usr/bin/env python3
"""Convert a KGSL (Adreno 6xx, msm-4.19 downstream kernel) GPU snapshot
(/sys/class/kgsl/kgsl-3d0/snapshot/dump) into the msm devcoredump text form
that Mesa's crashdec reads.  crashdec only goes devcoredump -> KGSL, not back.

usage: kgsl2devcore.py dump.bin > dump.devcore ; crashdec -f dump.devcore

The snapshot is world-readable on the RP5 (Snapdragon 865, Adreno 650):
  adb exec-out cat /sys/class/kgsl/kgsl-3d0/snapshot/dump > dump.bin
KGSL keeps the first unread snapshot, so read it after every hang.

Building crashdec (Mesa 26.2.4, MIT; only the freedreno tools):
  python3 -m venv venv && venv/bin/pip install meson mako pyyaml packaging
  meson setup build -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dopengl=false -Dllvm=disabled -Dshared-glapi=disabled -Dtools=freedreno \
    -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dexpat=disabled
  ninja -C build src/freedreno/decode/crashdec
(lua and libarchive come from Mesa's meson wraps). On macOS: bison 3.x is
needed (/usr/bin/bison 2.3 is too old), sscanf has no %ms (crashdec.c's
vparseline needs a small shim), and cffdec.c's dump_bindless_descriptors
walks whole descriptor BOs per draw (cap desc_count, e.g. at 32, or one
snapshot takes many minutes).

Reading the result: "ESTIMATED CRASH LOCATION!" marks where the SQE is,
the CP_SQE_STAT block names the packet it is executing, and the draws
just before it carry the full register state and shader disassembly.
"""
import struct, sys, time

SHADER_TYPES = {  # kgsl enum a6xx_shader_obj (adreno_a6xx_snapshot.c)
    0x9: 'A6XX_TP0_TMO_DATA', 0xa: 'A6XX_TP0_SMO_DATA', 0xb: 'A6XX_TP0_MIPMAP_BASE_DATA',
    0x19: 'A6XX_TP1_TMO_DATA', 0x1a: 'A6XX_TP1_SMO_DATA', 0x1b: 'A6XX_TP1_MIPMAP_BASE_DATA',
    0x29: 'A6XX_SP_INST_DATA', 0x2a: 'A6XX_SP_LB_0_DATA', 0x2b: 'A6XX_SP_LB_1_DATA',
    0x2c: 'A6XX_SP_LB_2_DATA', 0x2d: 'A6XX_SP_LB_3_DATA', 0x2e: 'A6XX_SP_LB_4_DATA',
    0x2f: 'A6XX_SP_LB_5_DATA', 0x30: 'A6XX_SP_CB_BINDLESS_DATA', 0x31: 'A6XX_SP_CB_LEGACY_DATA',
    0x32: 'A6XX_SP_UAV_DATA', 0x33: 'A6XX_SP_INST_TAG', 0x34: 'A6XX_SP_CB_BINDLESS_TAG',
    0x35: 'A6XX_SP_TMO_UMO_TAG', 0x36: 'A6XX_SP_SMO_TAG', 0x37: 'A6XX_SP_STATE_DATA',
    0x49: 'A6XX_HLSQ_CHUNK_CVS_RAM', 0x4a: 'A6XX_HLSQ_CHUNK_CPS_RAM',
    0x4b: 'A6XX_HLSQ_CHUNK_CVS_RAM_TAG', 0x4c: 'A6XX_HLSQ_CHUNK_CPS_RAM_TAG',
    0x4d: 'A6XX_HLSQ_ICB_CVS_CB_BASE_TAG', 0x4e: 'A6XX_HLSQ_ICB_CPS_CB_BASE_TAG',
    0x50: 'A6XX_HLSQ_CVS_MISC_RAM', 0x51: 'A6XX_HLSQ_CPS_MISC_RAM',
    0x52: 'A6XX_HLSQ_INST_RAM', 0x53: 'A6XX_HLSQ_GFX_CVS_CONST_RAM',
    0x54: 'A6XX_HLSQ_GFX_CPS_CONST_RAM', 0x55: 'A6XX_HLSQ_CVS_MISC_RAM_TAG',
    0x56: 'A6XX_HLSQ_CPS_MISC_RAM_TAG', 0x57: 'A6XX_HLSQ_INST_RAM_TAG',
    0x58: 'A6XX_HLSQ_GFX_CVS_CONST_RAM_TAG', 0x59: 'A6XX_HLSQ_GFX_CPS_CONST_RAM_TAG',
    0x5a: 'A6XX_HLSQ_PWR_REST_RAM', 0x5b: 'A6XX_HLSQ_PWR_REST_TAG',
    0x60: 'A6XX_HLSQ_DATAPATH_META', 0x70: 'A6XX_HLSQ_FRONTEND_META',
    0x71: 'A6XX_HLSQ_INDIRECT_META', 0x72: 'A6XX_HLSQ_BACKEND_META',
}
CLUSTERS = {1: 'CLUSTER_FE', 2: 'CLUSTER_SP_VS', 3: 'CLUSTER_PC_VS', 4: 'CLUSTER_GRAS',
            5: 'CLUSTER_SP_PS', 6: 'CLUSTER_PS', 7: 'CLUSTER_VPC_PS'}
INDEXED = {0x908: 'CP_SQE_STAT', 0x90a: 'CP_DRAW_STATE', 0x90c: 'CP_ROQ_DBG',
           0x90e: 'CP_MEM_POOL_DBG', 0x910: 'CP_SQE_UCODE_DBG'}


def a85(words):
    out = []
    for w in words:
        if w == 0:
            out.append('z')
            continue
        c = []
        for _ in range(5):
            c.append(chr(33 + w % 85))
            w //= 85
        out.append(''.join(reversed(c)))
    return ''.join(out)


def u32s(b):
    n = len(b) // 4
    return struct.unpack_from('<%dI' % n, b, 0)


def main():
    d = open(sys.argv[1], 'rb').read()
    magic, gpuid, chipid = struct.unpack_from('<III', d, 0)
    assert magic == 0x504D0002, hex(magic)
    secs = []
    off = 12
    while off + 8 <= len(d):
        m, i, sz = struct.unpack_from('<HHI', d, off)
        assert m == 0xABCD, (off, hex(m))
        secs.append((i, d[off + 8:off + sz]))
        if i == 0xFFFF or sz < 8:
            break
        off += sz

    regs, rbs, bos, idx, shaders, mvc = [], [], [], [], [], []
    release, secs_time = '4.19', 0
    for i, b in secs:
        if i == 0x101:
            secs_time = struct.unpack_from('<I', b, 4)[0]
            # release follows the fixed fields; find it as a printable string
            p = b.find(b'4.')
            if p >= 0:
                release = b[p:p + 32].split(b'\0')[0].decode()
        elif i == 0x201:
            cnt = struct.unpack_from('<I', b, 0)[0]
            w = u32s(b[4:4 + cnt * 8])
            regs += list(zip(w[0::2], w[1::2]))
        elif i == 0x302:
            st, en, rbsize, wptr, rptr, cnt, tq, tr, ga, rid = struct.unpack_from('<iiiiiiIIQI', b, 0)
            data = u32s(b[44:44 + cnt * 4])
            rbs.append(dict(id=rid, iova=ga, wptr=wptr, rptr=rptr, size=rbsize * 4,
                            tq=tq, tr=tr, data=data))
        elif i == 0x402:
            ga, ptb, size = struct.unpack_from('<QQQ', b, 0)
            bos.append((ga, b[24:24 + size * 4]))
        elif i == 0xB02:
            t, ga, ptb, size = struct.unpack_from('<iQQQ', b, 0)
            bos.append((ga, b[28:28 + size * 4]))
        elif i == 0x501:
            ir, dr, st, cnt = struct.unpack_from('<IIiI', b, 0)
            idx.append((INDEXED.get(ir, 'IDX_%04x' % ir), u32s(b[16:16 + cnt * 4])))
        elif i == 0x901:
            t, size = struct.unpack_from('<ii', b, 0)
            if t == 10:  # SNAPSHOT_DEBUG_CP_ROQ
                idx.append(('CP_ROQ_DBG', u32s(b[8:8 + size * 4])))
        elif i == 0x1201:
            t, index, size = struct.unpack_from('<iii', b, 0)
            shaders.append((SHADER_TYPES.get(t, 'A6XX_UNKNOWN_%x' % t), index, u32s(b[12:12 + size * 4])))
        elif i == 0x1501:
            ctx, cl = struct.unpack_from('<ii', b, 0)
            w = u32s(b[8:])
            pairs, k = [], 0
            while k < len(w):
                hdr = w[k]; k += 1
                start, n = hdr & 0xFFFFF, (hdr >> 20) + 1
                for j in range(n):
                    if k >= len(w):
                        break
                    pairs.append((start + j, w[k])); k += 1
            mvc.append((ctx, CLUSTERS.get(cl, 'CLUSTER_%d' % cl), pairs))

    # dedupe bos by iova (keep largest)
    bd = {}
    for ga, data in bos:
        if ga not in bd or len(data) > len(bd[ga]):
            bd[ga] = data

    o = sys.stdout
    core = (chipid >> 24) & 0xff, (chipid >> 16) & 0xff, (chipid >> 8) & 0xff, chipid & 0xff
    gpu_id = core[0] * 100 + core[1] * 10 + core[2]
    o.write('kgsl snapshot converted by kgsl2devcore.py\n')
    o.write('kernel: %s\n' % release)
    o.write('module: kgsl\n')
    o.write('time: %d.0\n' % secs_time)
    o.write('comm: SDLActivity\n')
    o.write('revision: %d (%d.%d.%d.%d)\n' % ((gpu_id,) + core))
    o.write('ringbuffer:\n')
    for r in rbs:
        o.write('  - id: %d\n' % r['id'])
        o.write('    iova: 0x%016x\n' % r['iova'])
        o.write('    last-fence: %u\n' % r['tq'])
        o.write('    retired-fence: %u\n' % r['tr'])
        o.write('    rptr: %d\n' % r['rptr'])
        o.write('    wptr: %d\n' % r['wptr'])
        o.write('    size: %d\n' % r['size'])
        o.write('    data: !!ascii85 |\n     %s\n' % a85(r['data']))
    o.write('bos:\n')
    for ga in sorted(bd):
        data = bd[ga]
        o.write('  - iova: 0x%016x\n' % ga)
        o.write('    size: %d\n' % len(data))
        o.write('    data: !!ascii85 |\n     %s\n' % a85(u32s(data)))
    o.write('registers:\n')
    seen = set()
    for r, v in regs:
        if r in seen:
            continue
        seen.add(r)
        o.write('  - { offset: 0x%06x, value: 0x%08x }\n' % (r * 4, v))
    o.write('indexed-registers:\n')
    for name, w in idx:
        o.write('  - regs-name: %s\n' % name)
        o.write('    dwords: %d\n' % len(w))
        o.write('    data: !!ascii85 |\n     %s\n' % a85(w))
    o.write('shader-blocks:\n')
    for name, index, w in shaders:
        o.write('  - type: %s\n' % name)
        o.write('    - bank: %d\n' % index)
        o.write('      size: %d\n' % len(w))
        o.write('      data: !!ascii85 |\n        %s\n' % a85(w))
    o.write('clusters:\n')
    for ctx, name, pairs in mvc:
        o.write('  - cluster-name: %s\n' % name)
        o.write('    - context: %d\n' % ctx)
        for r, v in pairs:
            o.write('      - { offset: 0x%06x, value: 0x%08x }\n' % (r * 4, v))


if __name__ == '__main__':
    main()
