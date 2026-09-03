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


# --- typed arrays (explicit sizes / inferred sizes) ---

def test_typed_fixed_array_compiles_and_runs():
    ec, out, err = run_and_capture(
        "let a: int[3] = [1, 2, 3];\nprint a[2];\n"
    )
    assert ec == 0, err
    assert out == ["3"]


def test_typed_fixed_array_float():
    ec, out, err = run_and_capture(
        "let a: float[2] = [1.5, 2.5];\nprint a[1];\n"
    )
    assert ec == 0, err
    assert out == ["2.500000"]


def test_typed_open_array_infers_size():
    ec, out, err = run_and_capture(
        "let a: int[] = [4, 5, 6];\nprint a[2];\n"
    )
    assert ec == 0, err
    assert out == ["6"]


def test_typed_array_size_mismatch_rejected():
    ec, err = compile_only("let a: int[2] = [1, 2, 3];\n")
    assert ec != 0, "size mismatch should be rejected"
    assert "2 elements" in err


def test_typed_array_element_mismatch_rejected():
    ec, err = compile_only("let a: int[3] = [1.5, 2.5, 3.5];\n")
    assert ec != 0, "element type mismatch should be rejected"
    assert "int elements" in err


def test_typed_potpourri_rejected():
    ec, err = compile_only("let a: bool[1] = [1];\n")
    assert ec != 0, "bool array with int element should be rejected"


# --- arrays as function parameters and returns ---

def test_array_param_passed_by_value():
    ec, out, err = run_and_capture(
        "func sum(a: int[3]): int {\n"
        "    return a[0] + a[1] + a[2];\n"
        "}\n"
        "let x: int[3] = [1, 2, 3];\n"
        "print sum(x);\n"
    )
    assert ec == 0, err
    assert out == ["6"]


def test_array_param_does_not_share_storage():
    ec, out, err = run_and_capture(
        "func set(a: int[2]): int {\n"
        "    a[0] = 50;\n"
        "    return a[0];\n"
        "}\n"
        "let x: int[2] = [1, 2];\n"
        "print set(x);\n"
        "print x[0];\n"
    )
    assert ec == 0, err
    assert out == ["50", "1"]


def test_array_return_from_function():
    ec, out, err = run_and_capture(
        "func make(): int[3] {\n"
        "    return [7, 8, 9];\n"
        "}\n"
        "let m: int[3] = make();\n"
        "print m[2];\n"
    )
    assert ec == 0, err
    assert out == ["9"]


def test_open_array_param_rejected():
    ec, err = compile_only(
        "func f(a: int[]): int {\n"
        "    return a[0];\n"
        "}\n"
    )
    assert ec != 0, "open array parameter should be rejected"
    assert "fixed size" in err


def test_open_array_return_rejected():
    ec, err = compile_only(
        "func g(): int[] {\n"
        "    return [1, 2];\n"
        "}\n"
    )
    assert ec != 0, "open array return should be rejected"
    assert "fixed size" in err


# --- runtime bounds checking ---

def test_index_high_overflow_fails_at_runtime():
    ec, out, err = run_and_capture("let a: int[3] = [1, 2, 3];\nprint a[3];\n")
    assert ec == 1, "out-of-range index should terminate the program"
    assert out == ["array index 3 out of bounds (size 3)"]


def test_index_negative_fails_at_runtime():
    ec, out, err = run_and_capture("let a: int[3] = [1, 2, 3];\nprint a[-2];\n")
    assert ec == 1, "negative index should terminate the program"
    assert out == ["array index -2 out of bounds (size 3)"]


def test_index_last_valid_ok():
    ec, out, err = run_and_capture("let a: int[3] = [1, 2, 3];\nprint a[2];\n")
    assert ec == 0
    assert out == ["3"]


def test_in_bounds_write_ok():
    ec, out, err = run_and_capture(
        "let a: int[2] = [0, 0];\na[1] = 42;\nprint a[1];\n"
    )
    assert ec == 0
    assert out == ["42"]


def test_oob_write_fails_at_runtime():
    ec, out, err = run_and_capture(
        "let a: int[2] = [0, 0];\na[5] = 42;\n"
    )
    assert ec == 1, "out-of-range write should terminate the program"
    assert out == ["array index 5 out of bounds (size 2)"]


# --- for-in ---

def test_for_in_iterates_array():
    ec, out, err = run_and_capture(
        "let a: int[3] = [10, 20, 30];\nfor x in a { print x; }\n"
    )
    assert ec == 0, err
    assert out == ["10", "20", "30"]


def test_for_in_accumulation():
    ec, out, err = run_and_capture(
        "let a: int[4] = [1, 2, 3, 4];\n"
        "let s: int = 0;\n"
        "for x in a { s = s + x; }\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["10"]


def test_for_in_over_function_return():
    ec, out, err = run_and_capture(
        "func make(): int[3] { return [5, 6, 7]; }\n"
        "for y in make() { print y; }\n"
    )
    assert ec == 0, err
    assert out == ["5", "6", "7"]


def test_for_in_float_array():
    ec, out, err = run_and_capture(
        "let a: float[2] = [1.5, 2.5];\nfor f in a { print f; }\n"
    )
    assert ec == 0, err
    assert out == ["1.500000", "2.500000"]


def test_for_in_loop_var_scoped():
    ec, err = compile_only(
        "let a: int[2] = [1, 2];\nfor x in a { print x; }\nprint x;\n"
    )
    assert ec != 0, "loop variable should not leak out of the loop"
    assert "not found" in err


def test_for_in_over_scalar_rejected():
    ec, err = compile_only(
        "let n: int = 5;\nfor z in n { print z; }\n"
    )
    assert ec != 0, "for-in over a non-array should be rejected"
    assert "for-in requires an array" in err

