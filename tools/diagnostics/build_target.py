#!/usr/bin/env python3
"""Build a single CMake target in the unified build/pc tree using make_packages' CLEAN_ENV
(the MSB6001 duplicate-'}' workaround). Read-only helper: imports make_packages without
running its main(). Usage: python glm-toolkit/build_target.py [target]
Default target: rincast-core-tests. Not a judge script; changes no criterion.
"""
import importlib.util
import subprocess
import sys
from pathlib import Path

WS = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("mp", WS / "glm-toolkit" / "make_packages.py")
mp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mp)

target = sys.argv[1] if len(sys.argv) > 1 else "rincast-core-tests"
cmd = [mp.CMAKE, "--build", str(mp.BUILD), "--config", "Release", "--target", target,
       "--parallel", "--", "/nodeReuse:false"]
p = subprocess.run(cmd, env=mp.CLEAN_ENV, capture_output=True, text=True,
                   encoding="utf-8", errors="replace")
sys.stdout.write(p.stdout[-6000:])
sys.stderr.write(p.stderr[-3000:])
print(f"\nBUILD_TARGET={target} EXIT={p.returncode}")
sys.exit(p.returncode)