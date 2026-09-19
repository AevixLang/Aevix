"""
Memory model contract tests.

Each test maps to one rule of the model. Only rules the current compiler
+ runtime already enforce are asserted here (the contract forbids red
tests: a test is added the moment its rule becomes enforceable).

Currently ENFORCED and asserted:
  - no-escape: a slice allocated in an epoch cannot be assigned to an
    outer variable; returning from inside an epoch is rejected;
    using/passing the slice to inner functions is fine.
  - no-uaf: arena rollback reuses memory and re-zeros it (deterministic
    stale reads instead of UB).
  - no-uninit: new T[n] is zeroed by the arena allocator.
  - no-oob: fixed-array and slice index checks abort on out-of-range.

KNOWN GAPS (NOT tested yet, negative tests land with sema 1.3):
  - writing an epoch slice into an outer struct field compiles today
    (memory_safety.md rule no-escape-4);
  - returning a copy of epoch data cannot yet be expressed (blanket ban).
"""
import os
import subprocess
import tempfile

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
BACKEND_BIN = os.path.join(ROOT, "backend", "build", "aevix-backend")
RUNTIME_OBJ = os.path.join(ROOT, "backend", "build", "runtime.o")
LLC = "/opt/homebrew/opt/llvm/bin/llc"
CORPUS = os.path.join(os.path.dirname(__file__), "corpus")


def compile_only(code):
    """Compile a snippet; returns (exit_code, error_text)."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    ast_json = os.path.join(FRONTEND_DIR, "ast.json")
    try:
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        if fe.returncode != 0:
            return fe.returncode, fe.stderr
        be = subprocess.run(
            [BACKEND_BIN, os.path.join(FRONTEND_DIR, "ast.json")],
            cwd=ROOT, capture_output=True, text=True,
        )
        return be.returncode, be.stderr
    finally:
        os.unlink(src)
        if os.path.exists(ast_json):
            os.unlink(ast_json)


def compile_and_run(code):
    """Full pipeline, returns (exit_code, stdout_lines, stderr)."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    workdir = tempfile.mkdtemp()
    try:
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src, "-o", os.path.join(workdir, "ast.json")],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        if fe.returncode != 0:
            return fe.returncode, [], fe.stderr
        be = subprocess.run(
            [BACKEND_BIN, os.path.join(workdir, "ast.json")],
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
        os.unlink(src)
        import shutil
        shutil.rmtree(workdir, ignore_errors=True)


# --- no-escape: slice cannot outlive its epoch ---

def test_no_escape_outer_var_assignment_rejected():
    ec, err = compile_only(
        "let outer: int[] = new int[2];\n"
        "epoch {\n"
        "    outer = new int[3];\n"
        "}\n"
    )
    assert ec != 0, "assigning an epoch slice to an outer var must be rejected"
    assert "epoch" in err.lower()


def test_no_escape_return_rejected():
    ec, err = compile_only(
        "func f() : int[] {\n"
        "    epoch {\n"
        "        let x = new int[2];\n"
        "        return x;\n"
        "    }\n"
        "    return [];\n"
        "}\n"
    )
    assert ec != 0, "returning arena-allocated value from epoch must be rejected"
    assert "epoch" in err.lower()


def test_no_escape_legal_use_inside_epoch():
    code, out, err = compile_and_run(
        "func total(a: int[]) : int {\n"
        "    let s = 0;\n"
        "    let i = 0;\n"
        "    while (i < len(a)) { s = s + a[i]; i = i + 1; }\n"
        "    return s;\n"
        "}\n"
        "hot {\n"
        "    epoch {\n"
        "        let m = new int[4];\n"
        "        m[0] = 5; m[1] = 7;\n"
        "        print total(m);\n"
        "    }\n"
        "}\n"
    )
    assert code == 0, err
    assert out == ["12"], out


# --- no-uaf: rollback reuses memory and re-zeros it ---

def test_no_uaf_arena_rollback_reuses_and_zeroes():
    code, out, err = compile_and_run(
        "hot {\n"
        "    epoch {\n"
        "        let a = new int[2];\n"
        "        a[0] = 42;\n"
        "    }\n"
        "    let b = new int[2];\n"
        "    print b[0];\n"
        "}\n"
    )
    assert code == 0, err
    assert out == ["0"], out


# --- no-uninit: new T[n] is zeroed ---

def test_no_uninit_new_is_zeroed():
    code, out, err = compile_and_run(
        "hot {\n"
        "    let a = new int[3];\n"
        "    print a[0];\n"
        "    print a[2];\n"
        "}\n"
    )
    assert code == 0, err
    assert out == ["0", "0"], out


# --- no-oob: runtime bounds checking ---

def test_no_oob_in_bounds_read():
    code, out, err = compile_and_run(
        "hot {\n"
        "    let a = [10, 20, 30];\n"
        "    print a[0];\n"
        "    print a[2];\n"
        "    let i = 1;\n"
        "    print a[i];\n"
        "}\n"
    )
    assert code == 0, err
    assert out == ["10", "30", "20"], out


def test_no_oob_fixed_array_write_fails():
    code, out, err = compile_and_run(
        "hot {\n"
        "    let a: int[2] = [1, 2];\n"
        "    a[5] = 9;\n"
        "}\n"
    )
    assert code != 0, "out-of-bounds fixed array write must abort"
    assert out and "out of bounds" in out[-1], out


def test_no_oob_slice_read_fails():
    code, out, err = compile_and_run(
        "func peek(a: int[], i: int) : int { return a[i]; }\n"
        "hot {\n"
        "    let s = new int[2];\n"
        "    print peek(s, 7);\n"
        "}\n"
    )
    assert code != 0, "out-of-bounds slice read must abort"
    assert out and "out of bounds" in out[-1], out