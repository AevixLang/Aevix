"""
Backend struct feature integration tests.

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


# --- struct basics ---

def test_struct_decl_and_literal_compiles():
    ec, err = compile_only(
        "struct Vec2 { x: float, y: float }\n"
        "let v = Vec2 { 1.0, 2.0 };\n"
    )
    assert ec == 0, err


def test_struct_field_read():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let p = Point { 3, 4 };\n"
        "print p.x;\n"
        "print p.y;\n"
    )
    assert ec == 0, err
    assert out == ["3", "4"]


def test_struct_field_write():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let p = Point { 1, 2 };\n"
        "p.x = 99;\n"
        "print p.x;\n"
    )
    assert ec == 0, err
    assert out == ["99"]


def test_struct_value_copy_semantics():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let p = Point { 1, 2 };\n"
        "let q = p;\n"
        "q.x = 50;\n"
        "print p.x;\n"
        "print q.x;\n"
    )
    assert ec == 0, err
    assert out == ["1", "50"]


def test_struct_print():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "print Point { 5, 6 };\n"
    )
    assert ec == 0, err
    assert out == ["{5, 6}"]


def test_struct_float_print():
    ec, out, err = run_and_capture(
        "struct Vec2 { x: float, y: float }\n"
        "print Vec2 { 1.0, 2.0 };\n"
    )
    assert ec == 0, err
    assert out == ["{1.000000, 2.000000}"]


def test_struct_string_field():
    ec, out, err = run_and_capture(
        "struct User { name: string, age: int, active: bool }\n"
        "let u = User { \"alice\", 30, true };\n"
        "print u;\n"
        "print u.name;\n"
    )
    assert ec == 0, err
    assert out == ["{alice, 30, 1}", "alice"]


# --- structs in functions ---

def test_struct_param():
    ec, out, err = run_and_capture(
        "struct Vec2 { x: float, y: float }\n"
        "func dist2(v: Vec2): float {\n"
        "    return v.x * v.x + v.y * v.y;\n"
        "}\n"
        "let v = Vec2 { 3.0, 4.0 };\n"
        "print dist2(v);\n"
    )
    assert ec == 0, err
    assert out == ["25.000000"]


def test_struct_return():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "func make(): Point {\n"
        "    return Point { 7, 8 };\n"
        "}\n"
        "let p = make();\n"
        "print p;\n"
    )
    assert ec == 0, err
    assert out == ["{7, 8}"]


# --- nested structs ---

def test_nested_struct_member_access():
    ec, out, err = run_and_capture(
        "struct Inner { a: int, b: int }\n"
        "struct Outer { inner: Inner, name: int }\n"
        "let o = Outer { Inner { 5, 6 }, 7 };\n"
        "print o.inner.b;\n"
        "o.inner.a = 55;\n"
        "print o.inner.a;\n"
        "print o.name;\n"
    )
    assert ec == 0, err
    assert out == ["6", "55", "7"]


# --- arrays of structs ---

def test_array_of_structs():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let pts = [Point { 1, 2 }, Point { 3, 4 }];\n"
        "print pts;\n"
        "print pts[1].y;\n"
        "pts[1].x = 30;\n"
        "print pts;\n"
    )
    assert ec == 0, err
    assert out == ["[{1, 2}, {3, 4}]", "4", "[{1, 2}, {30, 4}]"]


def test_for_in_over_struct_array():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let pts = [Point { 1, 2 }, Point { 3, 4 }];\n"
        "for p in pts { print p; }\n"
    )
    assert ec == 0, err
    assert out == ["{1, 2}", "{3, 4}"]


# --- struct literals with mixed types ---

def test_struct_literal_int_to_float_cast():
    ec, out, err = run_and_capture(
        "struct Vec2 { x: float, y: float }\n"
        "print Vec2 { 3, 4 };\n"
    )
    assert ec == 0, err
    assert out == ["{3.000000, 4.000000}"]


def test_struct_array_param():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "func sum(pts: Point[2]): int {\n"
        "    return pts[0].x + pts[1].y;\n"
        "}\n"
        "let a = [Point { 1, 2 }, Point { 3, 4 }];\n"
        "print sum(a);\n"
    )
    assert ec == 0, err
    assert out == ["5"]


# --- for-in grammar (regression: struct_lit vs block ambiguity) ---

def test_for_in_over_plain_variable_parses():
    ec, out, err = run_and_capture(
        "let a: int[3] = [10, 20, 30];\n"
        "for x in a { print x; }\n"
    )
    assert ec == 0, err
    assert out == ["10", "20", "30"]