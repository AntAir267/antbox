#!/usr/bin/env python3
"""SMAA.hlsl -> ../../src/video/smaa_hlsl.inc: the source with its comments
taken out, as a C string (one literal per line), the licence notice kept at
its head. Run from anywhere; python3 make_inc.py"""
import os
import re

here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, "SMAA.hlsl"), encoding="latin-1").read()
lic = open(os.path.join(here, "LICENSE.txt"), encoding="latin-1").read().strip()

code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
code = re.sub(r"//[^\n]*", "", code)
lines = [l.rstrip() for l in code.splitlines()]
lines = [l for l in lines if l.strip()]

out = ["/* smaa_hlsl.inc: SMAA.hlsl (third_party/smaa/, make_inc.py) without its",
       " * comments, as a C string.",
       " *",
       " * SMAA, https://github.com/iryoku/smaa:",
       " *"]
for l in lic.splitlines():
    out.append(" * " + l.rstrip() if l.strip() else " *")
out.append(" */")
out.append("static const char smaa_hlsl_source[] =")
for l in lines:
    esc = l.replace("\\", "\\\\").replace('"', '\\"')
    out.append('    "%s\\n"' % esc)
out[-1] += ";"
open(os.path.join(here, "..", "..", "src", "video", "smaa_hlsl.inc"), "w").write("\n".join(out) + "\n")
print(len(lines), "lines")
