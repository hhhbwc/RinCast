import os
#!/usr/bin/env python3
"""Build BOTH setup packages from a clean HEAD, then verify both.

  release : unmodified source, PC-side device approval intact (the official build)
  noauth  : patches/noauth-test.patch applied ONLY for this build, then reverted

The no-auth source change never enters the repository: a try/finally restores the
two files even if a build or pack step fails. Everything is verified by extracting
both packages and comparing sha256 + binary markers; mtime is never trusted.

Usage:  python glm-toolkit/make_packages.py [tag]
Default tag: nightly
Outputs: RinCast/dist/RinCast-1.2.1-<tag>-Setup.exe
         RinCast/dist/RinCast-1.2.1-<tag>-noauth-TEST-Setup.exe
         glm-toolkit/out/packages.json
"""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

WS = Path(__file__).resolve().parents[3]
REPO = WS / "RinCast"
BUILD = REPO / "build" / "pc"
BIN = BUILD / "bin" / "Release"
DIST = REPO / "dist"
PATCH = WS / "patches" / "noauth-test.patch"
BASE_STAGE = DIST / "RinCast"          # the known-good 27-file staging template
OUTJSON = WS / "glm-toolkit" / "out" / "packages.json"
CMAKE = r"C:\Program Files\CMake\bin\cmake.exe"

# MSB6001 workaround: this session's environment has a duplicated '}' variable.
CLEAN_ENV = {
    "SYSTEMROOT": r"C:\WINDOWS", "WINDIR": r"C:\WINDOWS", "SystemDrive": "C:",
    "COMSPEC": r"C:\WINDOWS\system32\cmd.exe",
    "TEMP": os.environ.get("TEMP", ""), "TMP": os.environ.get("TMP", ""),
    "USERPROFILE": os.environ.get("USERPROFILE", ""), "HOMEDRIVE": os.environ.get("HOMEDRIVE", ""), "HOMEPATH": os.environ.get("HOMEPATH", ""),
    "PATHEXT": ".COM;.EXE;.BAT;.CMD",
    "PATH": r"C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\Program Files\CMake\bin",
}
PATCHED_FILES = [
    "pc/streamer/src/net/stream_server.cpp",
    "pc/streamer/src/settings.cpp",
]
# All three targets must appear in the build log; "core-tests built" alone is not enough.
REQUIRED_TARGETS = ["rincast-core-tests.vcxproj", "rincast-streamer.vcxproj", "encode-bench.vcxproj"]


def run(cmd, env=None, check=True):
    # MSBuild output is not always valid in the locale code page (GBK here); a stray
    # byte crashed the reader thread and left stdout=None. Decode UTF-8, replace bad bytes.
    p = subprocess.run([str(c) for c in cmd], env=env, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if check and p.returncode != 0:
        raise RuntimeError(f"command failed ({p.returncode}): {cmd}\n{p.stdout[-2000:]}\n{p.stderr[-2000:]}")
    return p


def git(*args):
    return run(["git", "-C", str(REPO), *args]).stdout.strip()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build():
    p = run([CMAKE, "--build", str(BUILD), "--config", "Release", "--parallel", "--", "/nodeReuse:false"],
            env=CLEAN_ENV)
    log = p.stdout + p.stderr
    missing = [t for t in REQUIRED_TARGETS if t not in log]
    if missing:
        raise RuntimeError("build did not cover all three targets: " + ", ".join(missing))
    if "error" in log.lower() and "0 error" not in log.lower():
        # MSBuild prints "0 Error(s)" on success; only fail on a real error line.
        bad = [l for l in log.splitlines() if " error " in l.lower() or ": error" in l.lower()]
        if bad:
            raise RuntimeError("build errors:\n" + "\n".join(bad[:10]))
    return log


def stage(name):
    dst = DIST / f"stage-{name}"
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(BASE_STAGE, dst)
    for src, rel in [(BIN / "RinCast Streamer.exe", "RinCast Streamer.exe"),
                     (BIN / "rincast-audio-setup.exe", "rincast-audio-setup.exe"),
                     (BIN / "RinCast Setup.exe", "Uninstall RinCast.exe")]:
        shutil.copy2(src, dst / rel)
    for d in ("ui", "driver"):
        target = dst / d
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(BIN / d, target)
    return dst


def pack(stage_dir, out):
    run([BIN / "RinCast Setup.exe", "--pack", str(stage_dir), str(out)])
    if not Path(out).exists() or Path(out).stat().st_size < 100_000:
        raise RuntimeError("pack produced no plausible file: " + str(out))
    return out


def verify(pkg, stage_dir, expect_noauth, work):
    out = work / Path(pkg).stem
    if out.exists():
        shutil.rmtree(out)
    run([pkg, "--extract", str(out)])
    files = list(out.rglob("*"))
    n = sum(1 for f in files if f.is_file())
    if n != 27:
        raise RuntimeError(f"{pkg.name}: expected 27 files, got {n}")
    for rel in ("RinCast Streamer.exe", "ui/app.js", "ui/index.html", "ui/i18n.js", "uninstall.ps1"):
        if sha(out / rel) != sha(Path(stage_dir) / rel):
            raise RuntimeError(f"{pkg.name}: sha mismatch for {rel}")
    exe = (out / "RinCast Streamer.exe").read_bytes()
    has_marker = b"NOAUTH TEST BUILD" in exe
    if has_marker != expect_noauth:
        raise RuntimeError(f"{pkg.name}: NOAUTH marker={has_marker}, expected {expect_noauth}")
    for s in (b"pending_approval", b"gameFps", b"gpu wait", b"ENC 5s",
              b"profileControlsVideo", b"pcBitrateWins"):
        if s not in exe:
            raise RuntimeError(f"{pkg.name}: missing binary marker {s!r}")
    ui = (out / "ui" / "app.js").read_bytes()
    if b"gameFps" not in ui or b"profileControlsVideo" not in ui:
        raise RuntimeError(f"{pkg.name}: ui is stale")
    return {"files": n, "exe_sha": sha(out / "RinCast Streamer.exe"), "noauth_marker": has_marker}


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "nightly"
    dirty = git("status", "--short")
    if dirty:
        raise RuntimeError("worktree must be clean:\n" + dirty)
    if not PATCH.exists():
        raise RuntimeError("missing " + str(PATCH))
    run(["git", "-C", str(REPO), "apply", "--check", str(PATCH)])
    head = git("rev-parse", "--short", "HEAD")
    print(f"HEAD {head}; tag {tag}", flush=True)

    release_pkg = DIST / f"RinCast-1.2.1-{tag}-Setup.exe"
    noauth_pkg = DIST / f"RinCast-1.2.1-{tag}-noauth-TEST-Setup.exe"
    work = WS / "glm-toolkit" / "out" / "packcheck"
    work.mkdir(parents=True, exist_ok=True)
    result = {"head": head, "tag": tag}

    try:
        print("[1/6] build release (all three targets)", flush=True)
        build()
        print("[2/6] stage + pack release", flush=True)
        rel_stage = stage(tag + "-release")
        pack(rel_stage, release_pkg)

        print("[3/6] apply temporary no-auth patch", flush=True)
        run(["git", "-C", str(REPO), "apply", str(PATCH)])
        print("[4/6] build + stage + pack noauth", flush=True)
        build()
        no_stage = stage(tag + "-noauth")
        pack(no_stage, noauth_pkg)
    finally:
        print("[5/6] revert the no-auth source change", flush=True)
        run(["git", "-C", str(REPO), "checkout", "--", *PATCHED_FILES], check=False)
        left = git("status", "--short")
        print("    worktree after revert:", left or "(clean)", flush=True)
        if left:
            raise RuntimeError("worktree still dirty after revert:\n" + left)
        # Put the build outputs back in sync with HEAD so the next build is honest.
        build()

    print("[6/6] extract + verify both packages", flush=True)
    for pkg, st, expected in ((release_pkg, DIST / f"stage-{tag}-release", False),
                              (noauth_pkg, DIST / f"stage-{tag}-noauth", True)):
        info = verify(pkg, st, expected, work)
        info.update({"path": str(pkg), "bytes": pkg.stat().st_size, "sha256": sha(pkg)})
        result["release" if not expected else "noauth"] = info
        print(f"    {pkg.name}: {info['bytes']} B  sha {info['sha256'][:16]}…  "
              f"27 files OK  markers OK", flush=True)

    OUTJSON.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("summary:", OUTJSON, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
