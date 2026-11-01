#!/usr/bin/env python3
"""check_nnedi3.py <out prefix> <w> <h> [wrap] [upstream hook]

The GPU's NNEDI3 (texup_gpu_check's <out>_nnedi3_luma.f32 in, _model.f32
out: the 2x luma) against an independent NumPy evaluation of bjin's hook
file (third_party/upscale_shaders/upstream/nnedi3-nns16-win8x4.hook), its
two passes as mpv runs them: double_y, then double_x on that result, each
with its own weights and sample layout read from the file, and every read
through the job's edge rule (wrap: bit 0 u, bit 1 v; texup_gpu_check runs
with 3) on the image that pass reads (w x h, then w x 2h). Also reads
RECOMP_DEBUG=texup_dump's files (Vulkan): give the prefix up to
_luma.f32 / _model.f32 with its own wrap.

    --ref <file.f32>   compare <file.f32> (2h x 2w x 4 floats) instead of
                       the NumPy result: e.g. the old shader's model image
    --save <file.f32>  write the NumPy result (2h x 2w, one float a texel)
    --final <x2.bgra> <in.bgra>
                       also the job's colour result against a NumPy
                       recombine of the reference (the half-texel move onto
                       the centred grid, the bilinear colour plus the luma
                       difference, alpha nearest), in 8-bit steps, and the
                       result's offset from the title's texture: the shift
                       that best matches its 2x2-box-downsampled luma to the
                       title's (0 when the texel centres line up)

Passes when the largest error of the 2x luma is at most 1/255 (the
precision bar for 16-bit intermediates). NumPy only.
"""
import json
import re
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
UP = HERE.parent.parent / 'third_party' / 'upscale_shaders' / 'upstream' / 'nnedi3-nns16-win8x4.hook'


def f32(i):
    return struct.unpack('<f', struct.pack('<i', int(i)))[0]


def parse(block):
    """One hook: weights (neurons x samples), biases, the sample offsets."""
    n = int(re.search(r'float nnedi3\(vec4 samples\[(\d+)\]\)', block)[1])
    if f'mstd0 = sum / {4 * n}.0;' not in block or '5.0 * vsum / wsum * mstd1' not in block:
        sys.exit('NNEDI3: the hook\'s equations are not the expected ones')
    w1, w2, b = [], [], []
    for line in re.findall(r'^sum1=.*$', block, re.M):
        parts = line.split(';')
        def vec(s):
            out = np.zeros(4 * n)
            for m in re.finditer(r'W\((\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+)\)', s):
                i = int(m[1])
                out[4 * i:4 * i + 4] = [f32(m[k]) for k in range(2, 6)]
            return out
        w1.append(vec(parts[0]))
        w2.append(vec(parts[1]))
        ws = re.search(r'WS\((-?\d+),(-?\d+)\)', line)
        b.append((f32(ws[1]), f32(ws[2])))
    offs = [None] * (4 * n)
    for i, j, dx, dy in re.findall(r'^samples\[(\d+)\]\[(\d+)\] = HOOKED_texOff\(vec2\((-?[\d.]+), (-?[\d.]+)\)\).x;',
                                   block, re.M):
        offs[int(i) * 4 + int(j)] = (int(float(dx)), int(float(dy)))
    if None in offs:
        sys.exit('NNEDI3: the sample layout is not the expected one')
    double_y = 'double_y' in block.split('\n', 1)[0]
    return np.array(w1), np.array(w2), np.array(b), offs, double_y


def edge(idx, size, wrap):
    return idx % size if wrap else np.clip(idx, 0, size - 1)


def run_pass(img, hook, wrap):
    w1, w2, b, offs, double_y = hook
    h, w = img.shape
    ys, xs = np.arange(h), np.arange(w)
    s = np.stack([img[edge(ys + dy, h, wrap & 2)[:, None], edge(xs + dx, w, wrap & 1)[None, :]]
                  for dx, dy in offs], axis=-1)
    n = s.shape[-1]
    mstd0 = s.sum(-1) / n
    mstd1 = (s * s).sum(-1) / n - mstd0 * mstd0
    mstd2 = np.where(mstd1 >= 1.192092896e-7, 1.0 / np.sqrt(np.maximum(mstd1, 1e-30)), 0.0)
    mstd1 = mstd1 * mstd2
    sum1 = np.exp((s @ w1.T) * mstd2[..., None] + b[:, 0])
    sum2 = (s @ w2.T) * mstd2[..., None] + b[:, 1]
    vsum = (sum1 * (sum2 / (1.0 + np.abs(sum2)))).sum(-1)
    wsum = sum1.sum(-1)
    new = np.clip(mstd0 + 5.0 * vsum / wsum * mstd1, 0.0, 1.0)
    if double_y:
        out = np.empty((2 * h, w))
        out[0::2], out[1::2] = img, new
    else:
        out = np.empty((h, 2 * w))
        out[:, 0::2], out[:, 1::2] = img, new
    return out


def nnedi3(luma, wrap, src=UP):
    # double_y, combine_y, double_x, combine_x: the combines interleave
    # (even rows/columns the pass's input, odd the new ones: run_pass).
    blocks = re.split(r'(?=//!DESC)', src.read_text())[1:]
    if len(blocks) != 4 or 'combine_y' not in blocks[1] or 'combine_x' not in blocks[3]:
        sys.exit(f'NNEDI3: {len(blocks)} hooks, expected double/combine y, double/combine x')
    hy, hx = parse(blocks[0]), parse(blocks[2])
    if not hy[4] or hx[4]:
        sys.exit('NNEDI3: expected double_y, then double_x')
    return run_pass(run_pass(luma, hy, wrap), hx, wrap)


Y709 = np.array([0.2126, 0.7152, 0.0722])


def centred(y2, wrap):
    """NNEDI3's grid (texel k at source k/2) to the centred one: Catmull-Rom
    at q - 0.5 per axis, as recombine.comp mode 1."""
    cr = np.array([-1, 9, 9, -1]) / 16.0
    h2, w2 = y2.shape
    ys, xs = np.arange(h2), np.arange(w2)
    rows = sum(c * y2[edge(ys + d - 2, h2, wrap & 2)] for d, c in enumerate(cr))
    return sum(c * rows[:, edge(xs + d - 2, w2, wrap & 1)] for d, c in enumerate(cr))


def recombine(src, ym, wrap):
    """recombine.comp mode 1 in NumPy: src (h, w, 4) RGBA 0..1."""
    h, w = src.shape[:2]
    f = (np.arange(2 * h) + 0.5) * 0.5 - 0.5, (np.arange(2 * w) + 0.5) * 0.5 - 0.5
    y0, x0 = np.floor(f[0]).astype(int), np.floor(f[1]).astype(int)
    ty, tx = (f[0] - y0)[:, None, None], (f[1] - x0)[None, :, None]
    g = lambda yy, xx: src[edge(yy, h, wrap & 2)[:, None], edge(xx, w, wrap & 1)[None, :], :3]
    rgb = (g(y0, x0) * (1 - tx) + g(y0, x0 + 1) * tx) * (1 - ty) \
        + (g(y0 + 1, x0) * (1 - tx) + g(y0 + 1, x0 + 1) * tx) * ty
    rgb = np.clip(rgb + (np.clip(ym, 0, 1) - rgb @ Y709)[..., None], 0, 1)
    a = src[np.arange(2 * h)[:, None] // 2, np.arange(2 * w)[None, :] // 2, 3:]
    return np.concatenate([rgb, a], -1)


def offset(y_out, y_src, wrap):
    """The shift (x, y in source texels) that best maps the title's luma onto
    the 2x2-box-downsampled result (the box is centred, so it moves
    nothing). Sub-texel shifts by the Fourier phase, exact for a wrapping
    texture; a clamped axis is mirrored first."""
    h, w = y_src.shape
    d = y_out.reshape(h, 2, w, 2).mean((1, 3))
    best = []
    for axis, flag in ((1, wrap & 1), (0, wrap & 2)):
        a, b = y_src, d
        if not flag:
            a = np.concatenate([a, np.flip(a, axis)], axis)
            b = np.concatenate([b, np.flip(b, axis)], axis)
        n = a.shape[axis]
        k = np.fft.fftfreq(n)
        k = k[None, :] if axis == 1 else k[:, None]
        fa = np.fft.fft(a, axis=axis)
        errs = []
        for sft in np.arange(-0.5, 0.5001, 0.005):
            moved = np.real(np.fft.ifft(fa * np.exp(-2j * np.pi * k * sft), axis=axis))
            sl = (slice(None), slice(0, w)) if axis == 1 else (slice(0, h), slice(None))
            errs.append((float(((moved - b)[sl] ** 2).mean()), round(float(sft), 3)))
        best.append(min(errs)[1])
    return best


def main():
    args = sys.argv[1:]
    ref = save = final = None
    if '--final' in args:
        i = args.index('--final')
        final = args[i + 1], args[i + 2]
        del args[i:i + 3]
    if '--ref' in args:
        i = args.index('--ref')
        ref = args[i + 1]
        del args[i:i + 2]
    if '--save' in args:
        i = args.index('--save')
        save = args[i + 1]
        del args[i:i + 2]
    out, w, h = args[0], int(args[1]), int(args[2])
    wrap = int(args[3]) if len(args) > 3 else 3
    src = Path(args[4]) if len(args) > 4 else UP
    pre = out + '_nnedi3' if Path(out + '_nnedi3_luma.f32').exists() else out
    luma = np.fromfile(pre + '_luma.f32', dtype='<f4').reshape(h, w, 4)[..., 0].astype(np.float64)
    gpu = np.fromfile(pre + '_model.f32', dtype='<f4').reshape(2 * h, 2 * w, 4)[..., 0].astype(np.float64)
    if ref:
        exp = np.fromfile(ref, dtype='<f4').reshape(2 * h, 2 * w, 4)[..., 0].astype(np.float64)
    else:
        exp = nnedi3(luma, wrap, src)
    if save:
        exp.astype('<f4').tofile(save)
    err = np.abs(np.clip(exp, 0, 1) - np.clip(gpu, 0, 1))
    mse = float((err ** 2).mean())
    rep = dict(texels=int(exp.size), finite=bool(np.isfinite(gpu).all()),
               max_abs_error=float(err.max()), mean_abs_error=float(err.mean()),
               max_error_8bit=float(err.max() * 255),
               psnr_db=float(10 * np.log10(1.0 / mse)) if mse > 0 else float('inf'))
    ok = rep['finite'] and err.max() <= 1 / 255
    if final:
        src = np.fromfile(final[1], dtype='u1').reshape(h, w, 4)[..., [2, 1, 0, 3]] / 255.0
        out = np.fromfile(final[0], dtype='u1').reshape(2 * h, 2 * w, 4)[..., [2, 1, 0, 3]].astype(np.float64)
        want = recombine(src, centred(exp, wrap), wrap) * 255.0
        fe = np.abs(out[..., :3] - want[..., :3])
        rep.update(final_max_error_8bit=float(fe.max()), final_mean_error_8bit=float(fe.mean()),
                   final_alpha_exact=bool((out[..., 3] == np.round(want[..., 3])).all()),
                   offset_xy_texels=offset((out[..., :3] / 255.0) @ Y709, src[..., :3] @ Y709, wrap),
                   offset_xy_uncorrected=offset(np.clip(exp, 0, 1), src[..., :3] @ Y709, wrap))
        ok = ok and fe.max() <= 1.5 and rep['final_alpha_exact']
    print(json.dumps(rep))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
