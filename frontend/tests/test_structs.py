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
RUNTIME_OBJ = os.path.join(ROOT, "backend", "build", "runtime.o")
LLC = shutil.which("llc") or "/opt/homebrew/opt/llvm/bin/llc"
OPT = shutil.which("opt") or "/opt/homebrew/opt/llvm/bin/opt"


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
        opt_ll = os.path.join(workdir, "out.opt.ll")
        oc = subprocess.run([OPT, "-passes=default<O2>", "-S", ir, "-o", opt_ll],
                            capture_output=True, text=True)
        if oc.returncode != 0:
            return oc.returncode, [], oc.stderr
        obj = os.path.join(workdir, "out.o")
        lc = subprocess.run([LLC, "-filetype=obj", opt_ll, "-o", obj],
                            capture_output=True, text=True)
        if lc.returncode != 0:
            return lc.returncode, [], lc.stderr
        exe = os.path.join(workdir, "prog")
        cl = subprocess.run(["clang", obj, RUNTIME_OBJ, "-o", exe],
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
    assert out == ["{alice, 30, true}", "alice"]


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


# --- struct slices (typed literal, printing) ---

def test_typed_open_array_of_struct_literal():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let pts: Point[] = [Point { 1, 2 }, Point { 3, 4 }];\n"
        "print pts[0].x;\n"
        "print pts[1].y;\n"
    )
    assert ec == 0, err
    assert out == ["1", "4"]


def test_typed_open_array_of_structs_empty():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let pts: Point[] = [];\n"
        "print len(pts);\n"
    )
    assert ec == 0, err
    assert out == ["0"]


def test_print_open_array_of_structs():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let literal: Point[] = [Point { 1, 2 }, Point { 3, 4 }];\n"
        "let heap = new Point[2];\n"
        "heap[0] = Point { 5, 6 };\n"
        "heap[1] = Point { 7, 8 };\n"
        "print literal;\n"
        "print heap;\n"
    )
    assert ec == 0, err
    assert out == ["[{1, 2}, {3, 4}]", "[{5, 6}, {7, 8}]"]


# --- struct literals with mixed types ---

def test_struct_literal_int_to_float_cast():
    ec, out, err = run_and_capture(
        "struct Vec2 { x: float, y: float }\n"
        "print Vec2 { 3, 4 };\n"
    )
    assert ec == 0, err
    assert out == ["{3.000000, 4.000000}"]


# --- named struct literal fields ---

def test_struct_literal_named_fields_in_order():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "print Point { x: 1, y: 2 };\n"
    )
    assert ec == 0, err
    assert out == ["{1, 2}"]


def test_struct_literal_named_fields_reordered():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let p = Point { y: 4, x: 3 };\n"
        "print p.x;\n"
        "print p.y;\n"
        "print p;\n"
    )
    assert ec == 0, err
    assert out == ["3", "4", "{3, 4}"]


def test_struct_literal_named_fields_with_expressions():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let n = 7;\n"
        "print Point { y: n + 1, x: n };\n"
    )
    assert ec == 0, err
    assert out == ["{7, 8}"]


def test_struct_literal_named_fields_nested_and_mixed_types():
    ec, out, err = run_and_capture(
        "struct Inner { a: int, b: int }\n"
        "struct Outer { val: Inner, label: string }\n"
        "print Outer { label: \"t\", val: Inner { b: 2, a: 1 } };\n"
    )
    assert ec == 0, err
    assert out == ['{{1, 2}, t}']


def test_struct_literal_named_fields_with_slice_field():
    ec, out, err = run_and_capture(
        "struct Box { items: int[], label: string }\n"
        "let boxes: Box[] = [Box { label: \"a\", items: [1, 2] }];\n"
        "print boxes[0].label;\n"
        "print boxes[0].items;\n"
    )
    assert ec == 0, err
    assert out == ["a", "[1, 2]"]


def test_struct_literal_positional_fields_still_work():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let pts = [Point { 1, 2 }, Point { 3, 4 }];\n"
        "print pts[1];\n"
    )
    assert ec == 0, err
    assert out == ["{3, 4}"]


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

# --- stdlib imports ---

def test_std_text_import():
    ec, out, err = run_and_capture(
        'import "std:text";\n'
        'func main() {\n'
        '    print repeat("ab", 2);\n'
        '    print index_of("hello", "ll");\n'
        '}\n'
        'main();\n'
    )
    assert ec == 0, err
    assert out == ["abab", "2"]


def test_std_math_import():
    ec, out, err = run_and_capture(
        'import "std:math";\n'
        'func main() {\n'
        '    print pow_int(2, 8);\n'
        '    print gcd(20, 12);\n'
        '    print lcm(6, 8);\n'
        '}\n'
        'main();\n'
    )
    assert ec == 0, err
    assert out == ["256", "4", "24"]


def test_std_io_import():
    import tempfile
    import os
    with tempfile.NamedTemporaryFile(mode="w", suffix=".aev", delete=False) as f:
        f.write(
            'import "std:io";\n'
            'func main() {\n'
            '    print write_file("/tmp/aevtest/io_out.txt", "hello");\n'
            '}\n'
            'main();\n'
        )
        f.flush()
        src = f.name
    try:
        ec, out, err = run_and_capture(open(src).read())
        assert ec == 0, err
        assert out == ["true"]
        assert os.path.exists("/tmp/aevtest/io_out.txt")
        with open("/tmp/aevtest/io_out.txt") as r:
            assert r.read() == "hello"
    finally:
        os.unlink(src)


def test_std_all_import():
    ec, out, err = run_and_capture(
        'import "std:";\n'
        'func main() {\n'
        '    print is_empty("");\n'
        '    print pow_int(3, 3);\n'
        '    print gcd(15, 5);\n'
        '}\n'
        'main();\n'
    )
    assert ec == 0, err
    assert out == ["true", "27", "5"]


# --- match statement ---

def test_match_int_cases():
    ec, out, err = run_and_capture(
        "func main() {\n"
        "    let x = 2;\n"
        "    match (x) {\n"
        "        case 1: { print \"one\"; }\n"
        "        case 2: { print \"two\"; }\n"
        "        default: { print \"other\"; }\n"
        "    }\n"
        "}\n"
        "main();\n"
    )
    assert ec == 0, err
    assert out == ["two"]


def test_match_default():
    ec, out, err = run_and_capture(
        "func main() {\n"
        "    let x = 9;\n"
        "    match (x) {\n"
        "        case 1: { print \"one\"; }\n"
        "        default: { print \"default\"; }\n"
        "    }\n"
        "}\n"
        "main();\n"
    )
    assert ec == 0, err
    assert out == ["default"]


def test_match_multi_stmt():
    ec, out, err = run_and_capture(
        "func main() {\n"
        "    let x = 3;\n"
        "    match (x) {\n"
        "        case 3: { print \"three\"; print \"also\"; }\n"
        "    }\n"
        "}\n"
        "main();\n"
    )
    assert ec == 0, err
    assert out == ["three", "also"]


def test_match_enum_cases():
    ec, out, err = run_and_capture(
        "enum Color { red, green, blue }\n"
        "func name(c: Color) : string {\n"
        "    match (c) {\n"
        "        case Color.red: { return \"red\"; }\n"
        "        case Color.green: { return \"green\"; }\n"
        "        default: { return \"blue\"; }\n"
        "    }\n"
        "}\n"
        "func main() {\n    let c = Color.green;\n    print name(c);\n}\n"
        "main();\n"
    )
    assert ec == 0, err
    assert out == ["green"]
