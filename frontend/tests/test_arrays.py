"""
Backend array feature integration tests.

Runs the full pipeline (frontend .aev -> ast.json, backend -> IR -> object -> executable)
and checks both compile acceptance/rejection and runtime output values.
"""
import subprocess
import tempfile
import os
import shutil

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
BACKEND_BIN = os.path.join(ROOT, "backend", "build", "aevix-backend")
LLC = shutil.which("llc") or "/opt/homebrew/opt/llvm/bin/llc"


def compile_only(code):
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
            return fe.returncode, ""
        be = subprocess.run(
            [BACKEND_BIN, ast_json],
            cwd=ROOT, capture_output=True, text=True,
        )
        return be.returncode, be.stderr
    finally:
        os.unlink(src)


def run_and_capture(code):
    """Compile and run, returning (exit_code, stdout_lines, error_text)."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    workdir = tempfile.mkdtemp()
    try:
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        if fe.returncode != 0:
            return fe.returncode, [], fe.stderr
        # backend reads frontend's ast.json via shared file
        be = subprocess.run(
            [BACKEND_BIN, os.path.join(FRONTEND_DIR, "ast.json")],
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
        cl = subprocess.run(["clang", obj, "-o", exe],
                            capture_output=True, text=True)
        if cl.returncode != 0:
            return cl.returncode, [], cl.stderr
        run = subprocess.run([exe], capture_output=True, text=True)
        return run.returncode, run.stdout.splitlines(), run.stderr
    finally:
        os.unlink(src)


# --- compile acceptance / rejection ---

def test_array_literal_compiles():
    code, err = compile_only("let a = [1, 2, 3];\n")
    assert code == 0, err


def test_array_index_read_compiles():
    code, err = compile_only("let a = [1, 2];\nprint a[1];\n")
    assert code == 0, err


def test_array_index_write_compiles():
    code, err = compile_only("let a = [1, 2];\na[0] = 9;\n")
    assert code == 0, err


def test_nested_array_compiles():
    code, err = compile_only("let m = [[1, 2], [3, 4]];\nprint m[1][0];\n")
    assert code == 0, err


def test_float_array_compiles():
    code, err = compile_only("let f = [1.5, 2.5];\nprint f[0];\n")
    assert code == 0, err


def test_empty_array_rejected():
    code, err = compile_only("let a = [];\n")
    assert code != 0
    assert "empty" in err


def test_mixed_type_array_rejected():
    code, err = compile_only("let a = [1, 2.5];\n")
    assert code != 0
    assert "single type" in err


def test_index_non_array_rejected():
    code, err = compile_only("let x = 5;\nprint x[0];\n")
    assert code != 0
    assert "not an array" in err


def test_print_whole_array_rejected():
    code, err = compile_only("let a = [1, 2];\nprint a;\n")
    assert code != 0
    assert "array" in err


# --- runtime value verification ---

def test_array_read_value():
    ec, out, err = run_and_capture("let a = [10, 20, 30];\nprint a[1];\n")
    assert ec == 0, err
    assert out == ["20"]


def test_array_write_value():
    ec, out, err = run_and_capture(
        "let a = [1, 2];\na[0] = 99;\nprint a[0];\nprint a[1];\n"
    )
    assert ec == 0, err
    assert out == ["99", "2"]


def test_array_loop_sum():
    ec, out, err = run_and_capture(
        "let a = [1, 2, 3, 4];\n"
        "let sum = 0;\n"
        "for (let i = 0; i < 4; i = i + 1) {\n"
        "    sum = sum + a[i];\n"
        "}\n"
        "print sum;\n"
    )
    assert ec == 0, err
    assert out == ["10"]


def test_nested_array_value():
    ec, out, err = run_and_capture(
        "let m = [[1, 2], [3, 4]];\nprint m[1][1];\n"
    )
    assert ec == 0, err
    assert out == ["4"]


def test_array_copy_semantics():
    ec, out, err = run_and_capture(
        "let a = [1, 2];\nlet b = a;\na[0] = 100;\nprint b[0];\n"
    )
    assert ec == 0, err
    assert out == ["1"]
