#!/usr/bin/env python3
"""upscale_wrap.py - the texture upscalers' compute shaders from the upstream files.

    tools/shaders/upscale_wrap.py [<upscale_shaders dir>]

Reads the unmodified upstream shaders in <dir>/upstream (default
third_party/upscale_shaders) and writes, beside them, the files the
backends load at runtime (src/kernel/nv2a_texup.h):

  <name>.comp    GLSL 450 compute shaders (Vulkan compiles these at runtime
                 with glslang);
  <name>.metal   the same through glslangValidator -> SPIR-V -> spirv-cross
                 (Metal compiles MSL at runtime; it has no SPIR-V path);
  VERSION        a hash of every generated file: part of the disk cache's
                 key, so a changed shader never meets an old result.

The passes:

  luma           BT.709 luma of the title's texture (gamma-coded RGB, as
                 the models were trained on video luma), one channel.
  fsrcnnx_00..12 FSRCNNX_x2_8-0-4-1.glsl's 13 hooks before its aggregation,
                 each wrapped as a compute pass: the weights and equations are
                 the upstream text unchanged; LUMA_texOff and friends read the
                 previous pass's image with the texture's own edge rule (wrap
                 or clamp per axis) instead of mpv's clamped sampler.
  nnedi3         bjin's nnedi3-nns16-win8x4.hook (mpv-prescalers) in one 2x
                 compute pass of our own: its network function is the
                 upstream text unchanged; double_y and double_x (the same
                 network, the window transposed, which the generator
                 checks) run in one work group over a 16x16 input tile,
                 the double_y strip in shared memory, every read with the
                 texture's edge rule (on w x 2h between the two passes, as
                 mpv's sampler does).
  recombine      the model's 2x luma back to colour: the bilinear 2x colour
                 moved by the model's luma minus the bilinear's (the colour
                 rule of the offline comparison the owner judged); alpha is
                 the title's, nearest, never the model's. Mode 0 takes the
                 luma from FSRCNNX's 4-channel sub-pixel image (its upstream
                 aggregation pass, done here), mode 1 from NNEDI3's 2x luma,
                 moved by half an output texel onto the centred grid (NNEDI3
                 keeps the title's texels at even positions; mpv corrects
                 that with OFFSET).

Every pass has the same push constants (origin, size, wrap, mode): a chunk
of rows is one dispatch with its origin. Bindings: inputs 0..3, the output
image 4, push constants at MSL buffer 0.

Needs glslangValidator and spirv-cross on the PATH (Homebrew: glslang,
spirv-cross). MIT, as the rest of the toolkit; the upstream shaders and
what is generated from them are LGPL-3.0-or-later (NOTICE.md there).
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT = HERE.parent.parent / 'third_party' / 'upscale_shaders'

COMMON = '''#version 450
layout(push_constant) uniform UpParams {
    ivec2 origin;   // the chunk's first texel (threads) in the dispatch's domain
    ivec2 size;     // the input image's size
    ivec2 wrap;     // per axis: 1 wrap (repeat), 0 clamp to the edge
    int   mode;
    int   pad0;
} pc;

ivec2 edge(ivec2 p, ivec2 s)
{
    ivec2 c = clamp(p, ivec2(0), s - ivec2(1));
    ivec2 w = ((p % s) + s) % s;
    return ivec2(pc.wrap.x != 0 ? w.x : c.x, pc.wrap.y != 0 ? w.y : c.y);
}
'''

LUMA = COMMON + '''
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 4, rgba16f) writeonly uniform image2D destination;

void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy) + pc.origin;
    if (any(greaterThanEqual(p, pc.size)))
        return;
    vec3 c = texelFetch(source, p, 0).rgb;
    imageStore(destination, p, vec4(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0, 0.0, 1.0));
}
'''

RECOMBINE = COMMON + '''
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1, rgba16f) readonly uniform image2D ymodel;
layout(set = 0, binding = 4, rgba8) writeonly uniform image2D destination;

vec4 src(ivec2 p)
{
    return texelFetch(source, edge(p, pc.size), 0);
}

void main()
{
    ivec2 q = ivec2(gl_GlobalInvocationID.xy) + pc.origin;
    if (any(greaterThanEqual(q, pc.size * 2)))
        return;
    float ym;
    if (pc.mode == 0) {
        vec4 s = imageLoad(ymodel, q >> 1);
        int i = (q.x & 1) * 2 + (q.y & 1);
        ym = i == 0 ? s.x : i == 1 ? s.y : i == 2 ? s.z : s.w;
    } else {
        /* NNEDI3 keeps the title's texels at even positions: its texel k
         * sits at source k/2, a quarter texel from the centred grid this
         * texture is drawn on (mpv corrects it with the hooks' OFFSET).
         * The centred texel q is at NNEDI3's q - 0.5 on both axes:
         * Catmull-Rom at the half-way point, taps q-2..q+1 with weights
         * (-1, 9, 9, -1) / 16 per axis, the edge rule on 2w x 2h. */
        const float cr[4] = float[4](-0.0625, 0.5625, 0.5625, -0.0625);
        ivec2 s2 = pc.size * 2;
        ym = 0.0;
        for (int v = 0; v < 4; v++) {
            float row = 0.0;
            for (int u = 0; u < 4; u++)
                row += cr[u] * imageLoad(ymodel, edge(q + ivec2(u - 2, v - 2), s2)).x;
            ym += cr[v] * row;
        }
    }
    vec2 f = (vec2(q) + 0.5) * 0.5 - 0.5;
    ivec2 i0 = ivec2(floor(f));
    vec2 t = f - vec2(i0);
    vec3 rgb = mix(mix(src(i0).rgb, src(i0 + ivec2(1, 0)).rgb, t.x),
                   mix(src(i0 + ivec2(0, 1)).rgb, src(i0 + ivec2(1, 1)).rgb, t.x), t.y);
    float yb = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = clamp(rgb + (clamp(ym, 0.0, 1.0) - yb), 0.0, 1.0);
    /* Alpha: the title's texel under this one, nearest; never filtered. */
    float a = texelFetch(source, q >> 1, 0).a;
    imageStore(destination, q, vec4(rgb, a));
}
'''


def run(*args):
    subprocess.run(args, check=True)


def build(out, name, src, written):
    comp = out / (name + '.comp')
    spv = out / (name + '.spv')
    msl = out / (name + '.metal')
    comp.write_text(src)
    run('glslangValidator', '-V', '--quiet', '-S', 'comp', '-o', str(spv), str(comp))
    run('spirv-cross', str(spv), '--msl', '--msl-version', '20100',
        '--msl-decoration-binding', '--output', str(msl))
    spv.unlink()
    text = msl.read_text()
    # The loader binds inputs at 0..3, the output at 4, the constants at 0.
    if 'buffer(0)' not in text:
        sys.exit(f'{msl}: push constants not at buffer(0)')
    written += [comp, msl]


def fsrcnnx(up):
    source = (up / 'FSRCNNX_x2_8-0-4-1.glsl').read_text()
    header = source.split('//!HOOK LUMA', 1)[0]
    blocks = re.split(r'(?=//!HOOK LUMA)', source)[1:]
    if len(blocks) != 14:
        sys.exit(f'FSRCNNX: {len(blocks)} hooks, expected 14')
    out = []
    names = []                 # SAVE name of each pass
    for i, block in enumerate(blocks[:13]):
        inputs = re.findall(r'^//!BIND (\w+)', block, re.M)
        save = re.search(r'^//!SAVE (\w+)', block, re.M)[1]
        names.append(save)
        g = header + COMMON + '\nlayout(local_size_x = 16, local_size_y = 16) in;\n'
        for n, name in enumerate(inputs):
            g += (f'layout(set = 0, binding = {n}, rgba16f) readonly uniform image2D {name};\n'
                  f'#define {name}_texOff(off) imageLoad({name}, edge(ivec2(gl_GlobalInvocationID.xy)'
                  f' + pc.origin + ivec2(off), pc.size))\n')
        g += 'layout(set = 0, binding = 4, rgba16f) writeonly uniform image2D destination;\n'
        g += '// The upstream hook, unchanged:\n'
        g += block[block.index('vec4 hook()'):]
        g += ('\nvoid main()\n{\n    ivec2 p = ivec2(gl_GlobalInvocationID.xy) + pc.origin;\n'
              '    if (any(greaterThanEqual(p, pc.size)))\n        return;\n'
              '    imageStore(destination, p, hook());\n}\n')
        out.append((f'fsrcnnx_{i:02}', g, inputs, save))
    # The last hook is the aggregation into the 2x luma: recombine mode 0.
    if 'SUBCONV1' not in blocks[13]:
        sys.exit('FSRCNNX: the aggregation does not read SUBCONV1')
    return header, out


NNEDI3_HOOK = 'nnedi3-nns16-win8x4.hook'

NNEDI3 = r"""
// NNEDI3 2x in one pass: a work group takes TS x TS input texels and
// writes their 2TS x 2TS output. Phase 1 loads the luma it needs (with the
// texture's edge rule) into shared memory; phase 2 makes the strip of the
// 2x-height image (bjin's double_y: even rows the input's, odd rows the
// network's) that this tile's double_x reads, into shared memory; phase 3
// runs double_x (the same network on the transposed window) and writes
// 2 x 2 texels a thread. The intermediate's edge rule is applied on its own
// size (w x 2h), as mpv's sampler does between the two hooks; strip cells
// outside it are remapped and made from global memory.
#define TS 16
#define IN_W (TS + 10)          /* input columns x0-4 .. x0+TS+5 */
#define IN_H (TS + 7)           /* input rows    y0-3 .. y0+TS+3 */
#define Y1_W (TS + 3)           /* strip columns x0-1 .. x0+TS+1 */
#define Y1_H (2 * TS + 7)       /* strip rows   2y0-3 .. 2y0+2TS+3 */
#define Y1_ODD (TS + 4)         /* odd strip rows (strip index k even) */
layout(local_size_x = TS, local_size_y = TS) in;
layout(set = 0, binding = 0, rgba16f) readonly uniform image2D source;
layout(set = 0, binding = 4, rgba16f) writeonly uniform image2D destination;
shared float s_in[IN_H * IN_W];
shared float s_y1[Y1_H * Y1_W];

float src_luma(ivec2 p)
{
    return imageLoad(source, edge(p, pc.size)).x;
}

// bjin's network (the double_y hook's function), unchanged:
@NNEDI3@
#undef T
#undef W
#undef WS

void main()
{
    ivec2 base = ivec2(gl_WorkGroupID.xy) * TS + pc.origin;
    ivec2 ysize = ivec2(pc.size.x, pc.size.y * 2);
    int lid = int(gl_LocalInvocationIndex);
    vec4 samples[8];
    for (int id = lid; id < IN_H * IN_W; id += TS * TS) {
        int ty = id / IN_W;
        s_in[id] = src_luma(base + ivec2(id - ty * IN_W - 4, ty - 3));
    }
    barrier();
    // Odd strip rows first (the network), then the even ones (copies), so
    // the last threads of the network's second round take copies.
    for (int id = lid; id < Y1_H * Y1_W; id += TS * TS) {
        int odd = id < Y1_ODD * Y1_W ? 1 : 0;
        int e = odd != 0 ? id : id - Y1_ODD * Y1_W;
        int k = (e / Y1_W) * 2 + 1 - odd;
        int c = e % Y1_W;
        ivec2 q = ivec2(base.x - 1 + c, 2 * base.y - 3 + k);
        ivec2 m = edge(q, ysize);
        float v;
        if (m == q) {
            int tx = q.x - base.x + 4, ty = (q.y >> 1) - base.y + 3;
            if (odd == 0) {
                v = s_in[ty * IN_W + tx];
            } else {
                for (int i = 0; i < 8; i++)
                    for (int j = 0; j < 4; j++)
                        samples[i][j] = s_in[(ty + j - 1) * IN_W + tx + i - 3];
                v = nnedi3(samples);
            }
        } else if ((m.y & 1) == 0) {
            v = src_luma(ivec2(m.x, m.y >> 1));
        } else {
            for (int i = 0; i < 8; i++)
                for (int j = 0; j < 4; j++)
                    samples[i][j] = src_luma(ivec2(m.x + i - 3, (m.y >> 1) + j - 1));
            v = nnedi3(samples);
        }
        s_y1[k * Y1_W + c] = v;
    }
    barrier();
    ivec2 l = ivec2(gl_LocalInvocationID.xy);
    ivec2 p = base + l;
    if (any(greaterThanEqual(p, pc.size)))
        return;
    for (int dy = 0; dy < 2; dy++) {
        int k = 2 * l.y + dy + 3, c = l.x + 1;
        // double_x's window is double_y's transposed (checked by the
        // generator): samples[i][j] at strip (x + j - 1, y + i - 3).
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++)
                samples[i][j] = s_y1[(k + i - 3) * Y1_W + c + j - 1];
        float o = nnedi3(samples);
        imageStore(destination, ivec2(2 * p.x, 2 * p.y + dy), vec4(s_y1[k * Y1_W + c], 0.0, 0.0, 1.0));
        imageStore(destination, ivec2(2 * p.x + 1, 2 * p.y + dy), vec4(o, 0.0, 0.0, 1.0));
    }
}
"""


def nnedi3_hook(block):
    """One double_* hook: its network's text, the weights by sample offset."""
    fn = re.search(r'^float nnedi3\(vec4 samples\[8\]\) \{\n.*?^\}  // nnedi3\n', block, re.M | re.S)
    if not fn:
        sys.exit('NNEDI3: no nnedi3() with 8 samples (win8x4) in the hook')
    offs = {}
    for i, j, dx, dy in re.findall(r'^samples\[(\d)\]\[(\d)\] = HOOKED_texOff\(vec2\((-?[\d.]+), (-?[\d.]+)\)\)\.x;',
                                   block, re.M):
        offs[int(i) * 4 + int(j)] = (int(float(dx)), int(float(dy)))
    weights = []
    for line in re.findall(r'^sum1=.*$', fn[0], re.M):
        w = {}
        for half, part in enumerate(line.split(';')[:2]):
            for m in re.finditer(r'W\((\d),(-?\d+),(-?\d+),(-?\d+),(-?\d+)\)', part):
                for n in range(4):
                    w[(half, offs[int(m[1]) * 4 + n])] = m[2 + n]
        weights.append((w, re.search(r'WS\((-?\d+),(-?\d+)\)', line).groups()))
    return fn[0], offs, weights


def nnedi3(up):
    source = (up / NNEDI3_HOOK).read_text()
    blocks = re.split(r'(?=//!DESC)', source)[1:]
    names = [re.match(r'//!DESC NNEDI3 \((\w+),', b)[1] for b in blocks]
    if names != ['double_y', 'combine_y', 'double_x', 'combine_x']:
        sys.exit(f'NNEDI3: hooks {names}, expected double_y combine_y double_x combine_x')
    fy, oy, wy = nnedi3_hook(blocks[0])
    fx, ox, wx = nnedi3_hook(blocks[2])
    # The shader's sample layouts: double_y samples[i][j] at (i - 3, j - 1);
    # double_x is the same network with the window transposed.
    if oy != {i * 4 + j: (i - 3, j - 1) for i in range(8) for j in range(4)}:
        sys.exit('NNEDI3: double_y\'s window is not the expected 8x4')
    for (w1, b1), (w2, b2) in zip(wy, wx):
        if b1 != b2 or w2 != {(h, (dy, dx)): v for (h, (dx, dy)), v in w1.items()}:
            sys.exit('NNEDI3: double_x is not double_y transposed')
    if len(wy) != len(wx):
        sys.exit('NNEDI3: the two hooks have different networks')
    return ('// Derived files: see NOTICE.md. tools/shaders/upscale_wrap.py wraps the\n'
            f'// network of upstream/{NNEDI3_HOOK} (bjin) in a single 2x compute pass.\n'
            + COMMON + NNEDI3.replace('@NNEDI3@', fy))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    up = out / 'upstream'
    written = []
    lic = ('// Generated by tools/shaders/upscale_wrap.py from upstream/; LGPL-3.0-or-later\n'
           '// (NOTICE.md, COPYING.LESSER). Edit the generator, not this file.\n')
    build(out, 'luma', lic + LUMA, written)
    build(out, 'recombine', lic + RECOMBINE, written)
    header, passes = fsrcnnx(up)
    for name, g, _, _ in passes:
        build(out, name, lic.replace('//', '//', 1) + g, written)
    build(out, 'nnedi3', lic + nnedi3(up), written)
    h = hashlib.sha256()
    for f in sorted(written):
        h.update(f.name.encode())
        h.update(f.read_bytes())
    (out / 'VERSION').write_text(h.hexdigest()[:16] + '\n')
    print(f'{len(written)} files in {out}, version {h.hexdigest()[:16]}')


if __name__ == '__main__':
    main()
