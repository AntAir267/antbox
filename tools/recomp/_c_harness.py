"""Compile a self-contained C fixture and run it, for tests that check lifted
code by executing it. Upstream keeps this in test_icall_caller_cleans.py
(d88cbeaf), which AntBox doesn't carry; the parity, flag and stack tests
ported from upstream import it from here."""
import shutil
import subprocess
import tempfile
from pathlib import Path


def _build_and_run(source):
    import pytest
    cc = shutil.which("cl") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    with tempfile.TemporaryDirectory(prefix="recomp-regression-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        c.write_text(source, encoding="utf-8")
        args = ([cc, "/nologo", "/W0", "/O2", str(c), "/Fe:" + str(exe)]
                if Path(cc).stem.lower() == "cl"
                else [cc, "-w", "-O2", str(c), "-o", str(exe), "-lm"])
        built = subprocess.run(args, cwd=tmp, capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr + source
        return subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True)
