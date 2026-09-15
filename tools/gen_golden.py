#!/usr/bin/env python3
"""
Regenerate golden files for the corpus test (frontend/tests/corpus).

For every corpus/*.aev this runs the full pipeline (frontend -> backend ->
LLVM -> clang -> run, with no arguments and empty stdin) and writes the
observed stdout to corpus/<name>.golden as the expected-output contract.

Usage (from repo root):
    venv/bin/python tools/gen_golden.py

Run it only when the CURRENT output is intentionally correct (e.g. a
deliberate language change) and commit the .golden updates in the same
commit as the change that caused them.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(__file__), "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
BACKEND_BIN = os.path.join(ROOT, "backend", "build", "aevix-backend")
RUNTIME_OBJ = os.path.join(ROOT, "backend", "build", "runtime.o")
LLC = shutil.which("llc") or "/opt/homebrew/opt/llvm/bin/llc"
CORPUS = os.path.join(FRONTEND_DIR, "tests", "corpus")


def build_and_run(src):
    """Full pipeline for one .aev file; returns (exit_code, stdout_lines, err)."""
    workdir = tempfile.mkdtemp()
    try:
        ast_json = os.path.join(workdir, "ast.json")
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src, "-o", ast_json],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        if fe.returncode != 0:
            return fe.returncode, [], fe.stderr
        be = subprocess.run(
            [BACKEND_BIN, ast_json],
            cwd=workdir, capture_output=True, text=True,
        )
        if be.returncode != 0:
            return be.returncode, [], be.stderr
        ir = os.path.join(workdir, "out.ll")
        with open(ir, "w") as fh:
            fh.write(be.stdout)
        obj = os.path.join(workdir, "out.o")
        lc = subprocess.run([LLC, "-filetype=obj", ir, "-o", obj],
                            capture_output=True, text=True)
        if lc.returncode != 0:
            return lc.returncode, [], lc.stderr
        exe = os.path.join(workdir, "prog")
        cl = subprocess.run(["clang", obj, RUNTIME_OBJ, "-o", exe],
                            capture_output=True, text=True)
        if cl.returncode != 0:
            return cl.returncode, [], cl.stderr
        run = subprocess.run([exe], cwd=workdir, capture_output=True, text=True)
        return run.returncode, run.stdout.splitlines(), run.stderr
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def main():
    changed = 0
    for name in sorted(os.listdir(CORPUS)):
        if not name.endswith(".aev"):
            continue
        src = os.path.join(CORPUS, name)
        code, out, err = build_and_run(src)
        if code != 0:
            print(f"SKIP {name}: exit {code}\n{err}")
            continue
        golden = os.path.join(CORPUS, name[:-4] + ".golden")
        with open(golden, "w") as fh:
            fh.write("\n".join(out) + ("\n" if out else ""))
        print(f"WROTE {os.path.basename(golden)} ({len(out)} lines)")
        changed += 1
    print(f"\n{changed} goldens written.")


if __name__ == "__main__":
    main()