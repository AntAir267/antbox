#!/usr/bin/env python3
"""check_fsrcnnx.py <out prefix> <w> <h> [wrap] [upstream glsl]

The GPU's FSRCNNX (texup_gpu_check's <out>_fsrcnnx_luma.f32 in, _model.f32
out: SUBCONV1, the last hook's input) against an independent NumPy
evaluation of the upstream file's weights and equations on the same luma,
with the job's edge rule (wrap: bit 0 u, bit 1 v; texup_gpu_check runs with
3, both axes). Also reads RECOMP_DEBUG=texup_dump's files (Vulkan): give the
prefix up to _luma.f32 / _model.f32 with its own wrap; the files of
texup_gpu_check are <prefix>_fsrcnnx_luma.f32 and _model.f32.
Passes when the largest error of the 2x luma is at most 1/255 (the
precision bar for 16-bit intermediates). NumPy only.
"""
import json
import re
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
UP = HERE.parent.parent / 'third_party' / 'upscale_shaders' / 'upstream' / 'FSRCNNX_x2_8-0-4-1.glsl'


def vector(s):
    return np.array([float(x) for x in s.split(',')], dtype=np.float64)


def main():
    out, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    wrap = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    src = Path(sys.argv[5]) if len(sys.argv) > 5 else UP
    pre = out + '_fsrcnnx' if Path(out + '_fsrcnnx_luma.f32').exists() else out
    luma = np.fromfile(pre + '_luma.f32', dtype='<f4').reshape(h, w, 4).astype(np.float64)
    gpu = np.fromfile(pre + '_model.f32', dtype='<f4').reshape(h, w, 4).astype(np.float64)
    images = {'LUMA': luma}
    blocks = re.split(r'(?=//!HOOK LUMA)', src.read_text())[1:]

    def shifted(name, coords):
        x, y = (0, 0) if coords == '0' else map(int, coords[5:-1].split(','))
        im = images[name]
        ys = np.arange(h) + y
        xs = np.arange(w) + x
        ys = ys % h if wrap & 2 else np.clip(ys, 0, h - 1)
        xs = xs % w if wrap & 1 else np.clip(xs, 0, w - 1)
        return im[ys[:, None], xs[None, :]]

    for block in blocks[:-1]:
        name = re.search(r'^//!SAVE (\w+)', block, re.M)[1]
        bias = vector(re.search(r'vec4 res = vec4\(([^)]+)\);', block)[1])
        res = np.broadcast_to(bias, (h, w, 4)).copy()
        body = block[block.index('vec4 hook()'):]
        for line in body.splitlines():
            if not line.startswith('res += '):
                continue
            m = re.fullmatch(r'res \+= mat4(?:x4)?\(([^)]+)\) \* (\w+)_texOff\((vec2\([^)]+\)|0)\);', line)
            if m:
                res += shifted(m[2], m[3]) @ vector(m[1]).reshape(4, 4)
                continue
            m = re.fullmatch(r'res \+= vec4\(([^)]+)\) \* float\((\w+)_texOff\((vec2\([^)]+\)|0)\)\);', line)
            if m:
                res += shifted(m[2], m[3])[..., :1] * vector(m[1])
                continue
            m = re.fullmatch(r'res \+= (\w+)_texOff\((vec2\([^)]+\)|0)\);', line)
            if m:
                res += shifted(m[1], m[2])
                continue
            raise ValueError(line)
        prelu = re.search(r'res = max\(res, vec4\(0.0\)\) \+ vec4\(([^)]+)\) \* min\(res, vec4\(0.0\)\);', body)
        if prelu:
            res = np.maximum(res, 0) + vector(prelu[1]) * np.minimum(res, 0)
        images[name] = res
    exp = images['SUBCONV1']
    err = np.abs(np.clip(exp, 0, 1) - np.clip(gpu, 0, 1))
    rep = dict(texels=int(exp.size), finite=bool(np.isfinite(gpu).all()),
               max_abs_error=float(err.max()), mean_abs_error=float(err.mean()),
               max_error_8bit=float(err.max() * 255))
    print(json.dumps(rep))
    sys.exit(0 if rep['finite'] and err.max() <= 1 / 255 else 1)


if __name__ == '__main__':
    main()
