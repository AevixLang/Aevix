"""
Golden corpus integration test.

Every corpus/*.aev paired with corpus/*.golden is part of a living output
contract: the full pipeline (frontend -> backend -> LLVM -> clang -> run,
with no arguments and empty stdin) must produce stdout identical to the
.golden file.

Regenerating goldens (only after an intentional, verified output change):
    venv/bin/python tools/gen_golden.py
"""
import glob
import os
import shutil
import subprocess
import tempfile

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
BACKEND_BIN = os.path.join(ROOT, "backend", "build", "aevix-backend")
RUNTIME_OBJ = os.path.join(ROOT, "backend", "build", "runtime.o")
LLC = shutil.which("llc") or "/opt/homebrew/opt/llvm/bin/llc"
CORPUS = os.path.join(os.path.dirname(__file__), "corpus")


def _golden_pairs():
    pairs = []
    for src in sorted(glob.glob(os.path.join(CORPUS, "*.aev"))):
        golden = src[:-4] + ".golden"
        if os.path.exists(golden):
            pairs.append((src, golden))
    return pairs


def test_corpus_has_goldens():
    assert _golden_pairs(), "corpus/*.aev without *.golden"


def test_corpus_output_matches_goldens():
    pairs = _golden_pairs()
    assert pairs
    for src, golden in pairs:
        name = os.path.basename(src)
        with open(golden) as fh:
            expected = fh.read().splitlines()
        workdir = tempfile.mkdtemp(prefix="aevix-golden-")
        try:
            _assert_matches(src, name, expected, workdir)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)


def _assert_matches(src, name, expected, workdir):
    ast_json = os.path.join(workdir, "ast.json")
    fe = subprocess.run(
        [VENV_PYTHON, "-m", "src.main", src, "-o", ast_json],
        cwd=FRONTEND_DIR, capture_output=True, text=True,
    )
    assert fe.returncode == 0, f"{name}: frontend failed: {fe.stderr}"
    be = subprocess.run(
        [BACKEND_BIN, ast_json],
        cwd=workdir, capture_output=True, text=True,
    )
    assert be.returncode == 0, f"{name}: backend failed: {be.stderr}"
    ir = os.path.join(workdir, "out.ll")
    with open(ir, "w") as fh:
        fh.write(be.stdout)
    obj = os.path.join(workdir, "out.o")
    lc = subprocess.run([LLC, "-filetype=obj", ir, "-o", obj],
                        capture_output=True, text=True)
    assert lc.returncode == 0, f"{name}: llc failed: {lc.stderr}"
    exe = os.path.join(workdir, "prog")
    cl = subprocess.run(["clang", obj, RUNTIME_OBJ, "-o", exe],
                        capture_output=True, text=True)
    assert cl.returncode == 0, f"{name}: link failed: {cl.stderr}"
    run = subprocess.run([exe], cwd=workdir, capture_output=True, text=True)
    assert run.returncode == 0, f"{name}: exited {run.returncode}: {run.stderr}"
    assert run.stdout.splitlines() == expected, f"{name}: output mismatch"