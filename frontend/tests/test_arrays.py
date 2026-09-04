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
RUNTIME_OBJ = os.path.join(ROOT, "backend", "build", "runtime.o")
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


def run_and_capture(code, args=(), files=None, stdin=None):
    """Compile and run, returning (exit_code, stdout_lines, error_text).
    `args` are forwarded to the compiled program; `files` (name -> content)
    are written into the run directory before execution; `stdin` is fed to
    the program on stdin."""
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
        cl = subprocess.run(["clang", obj, RUNTIME_OBJ, "-o", exe],
                            capture_output=True, text=True)
        if cl.returncode != 0:
            return cl.returncode, [], cl.stderr
        for name, content in (files or {}).items():
            with open(os.path.join(workdir, name), "w") as fh:
                fh.write(content)
        run = subprocess.run([exe, *args], capture_output=True, text=True,
                             cwd=workdir, input=stdin)
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


def test_print_whole_array_outputs_bracket_format():
    ec, out, err = run_and_capture("let a = [1, 2];\nprint a;\n")
    assert ec == 0, err
    assert out == ["[1, 2]"]


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
    assert "size mismatch" in err.lower()


def test_typed_array_element_mismatch_rejected():
    ec, err = compile_only("let a: int[3] = [1.5, 2.5, 3.5];\n")
    assert ec != 0, "element type mismatch should be rejected"
    assert "mismatch" in err.lower()


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


def test_open_array_param_accepted():
    ec, err = compile_only(
        "func f(a: int[]) : int {\n"
        "    return a[0];\n"
        "}\n"
    )
    assert ec == 0, "open array parameter should be accepted: " + err


def test_open_array_return_accepted():
    ec, err = compile_only(
        "func g() : int[] {\n"
        "    return new int[5];\n"
        "}\n"
    )
    assert ec == 0, "open array return should be accepted: " + err


# --- new (arena allocation) ---

def test_new_array_read_write():
    ec, out, err = run_and_capture(
        "let a: int[] = new int[3];\n"
        "a[0] = 10; a[1] = 20; a[2] = 30;\n"
        "print a[0] + a[1] + a[2];\n"
        "print len(a);\n"
    )
    assert ec == 0, err
    assert out == ["60", "3"]


def test_new_array_zeroed():
    ec, out, err = run_and_capture(
        "let a = new int[4];\n"
        "print a[1];\n"
    )
    assert ec == 0, err
    assert out == ["0"]


def test_new_float_array():
    ec, out, err = run_and_capture(
        "let f = new float[2];\n"
        "f[1] = 2.5;\n"
        "print f[0];\n"
        "print f[1];\n"
    )
    assert ec == 0, err
    assert out == ["0.000000", "2.500000"]


def test_new_struct_array():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let pts = new Point[2];\n"
        "pts[0].x = 3;\n"
        "pts[0].y = 4;\n"
        "pts[1].x = 5;\n"
        "pts[1].y = 6;\n"
        "print pts[0].x + pts[0].y;\n"
        "print pts[1].y;\n"
        "print len(pts);\n"
    )
    assert ec == 0, err
    assert out == ["7", "6", "2"]


def test_new_runtime_size():
    ec, out, err = run_and_capture(
        "let n = 5;\n"
        "let a = new int[n];\n"
        "let i = 0;\n"
        "while (i < n) {\n"
        "    a[i] = i * i;\n"
        "    i = i + 1;\n"
        "}\n"
        "print len(a);\n"
        "print a[4];\n"
    )
    assert ec == 0, err
    assert out == ["5", "16"]


def test_new_runtime_size_expression():
    ec, out, err = run_and_capture(
        "let n = 3;\n"
        "let a = new float[n + 1];\n"
        "a[0] = 1.5;\n"
        "print a[n];\n"
        "print a[1];\n"
    )
    assert ec == 0, err
    assert out == ["0.000000", "0.000000"]


def test_new_runtime_size_struct():
    ec, out, err = run_and_capture(
        "struct Point { x: int, y: int }\n"
        "let k = 2;\n"
        "let pts = new Point[k];\n"
        "pts[0].x = 7;\n"
        "print pts[0].x;\n"
        "print len(pts);\n"
    )
    assert ec == 0, err
    assert out == ["7", "2"]


def test_new_zero_size_rejected():
    code, err = compile_only("let a = new int[0];\n")
    assert code != 0, "new int[0] should be rejected"
    assert "positive array size" in err


def test_open_array_return_and_use():
    ec, out, err = run_and_capture(
        "func make(): int[] {\n"
        "    let a = new int[3];\n"
        "    a[0] = 5; a[1] = 6; a[2] = 7;\n"
        "    return a;\n"
        "}\n"
        "let b = make();\n"
        "print b[0] + b[1] + b[2];\n"
        "print len(b);\n"
    )
    assert ec == 0, err
    assert out == ["18", "3"]


def test_open_array_return_literal():
    ec, out, err = run_and_capture(
        "func make(): int[] {\n"
        "    return [3, 4, 5];\n"
        "}\n"
        "let c = make();\n"
        "print c[2];\n"
    )
    assert ec == 0, err
    assert out == ["5"]


def test_print_open_array():
    ec, out, err = run_and_capture(
        "let a = new int[2];\n"
        "a[0] = 1; a[1] = 2;\n"
        "print a;\n"
    )
    assert ec == 0, err
    assert out == ["[1, 2]"]


def test_for_in_over_new_array():
    ec, out, err = run_and_capture(
        "let a = new int[3];\n"
        "a[0] = 2; a[1] = 4; a[2] = 6;\n"
        "let total = 0;\n"
        "for x in a {\n"
        "    total = total + x;\n"
        "}\n"
        "print total;\n"
    )
    assert ec == 0, err
    assert out == ["12"]


# --- strings (slices of i8) ---

def test_string_print_no_quotes():
    ec, out, err = run_and_capture('let s: string = "hello";\nprint s;\n')
    assert ec == 0, err
    assert out == ["hello"]


def test_string_len():
    ec, out, err = run_and_capture('let s: string = "hello";\nprint len(s);\n')
    assert ec == 0, err
    assert out == ["5"]


def test_string_index_char():
    ec, out, err = run_and_capture('let s: string = "hello";\nprint s[0];\nprint s[4];\n')
    assert ec == 0, err
    assert out == ["h", "o"]


def test_string_concat():
    ec, out, err = run_and_capture('let a: string = "foo";\nlet b = "bar";\nprint a + b;\nprint len(a + b);\n')
    assert ec == 0, err
    assert out == ["foobar", "6"]


def test_string_literal_concat():
    ec, out, err = run_and_capture('print "hello" + " world";\n')
    assert ec == 0, err
    assert out == ["hello world"]


def test_string_equality():
    ec, out, err = run_and_capture(
        'let s: string = "hi";\n'
        'if (s == "hi") { print "eq"; }\n'
        'if (s != "hi") { print "neq-wrong"; }\n'
        'if (s != "hx") { print "ne"; }\n'
        'if (s == "h") { print "eq-wrong"; }\n'
    )
    assert ec == 0, err
    assert out == ["eq", "ne"]


def test_string_concat_comparison():
    ec, out, err = run_and_capture(
        'if ("a" + "b" == "ab") { print "yes"; }\n'
    )
    assert ec == 0, err
    assert out == ["yes"]


def test_string_char_arithmetic():
    ec, out, err = run_and_capture(
        'let s: string = "abc";\nprint s[0] + 1;\n'
    )
    assert ec == 0, err
    assert out == ["98"]


def test_string_param_and_return():
    ec, out, err = run_and_capture(
        "func greet(p: string): string {\n"
        '    return p + "!";\n'
        "}\n"
        'print greet("hi");\n'
    )
    assert ec == 0, err
    assert out == ["hi!"]


def test_string_struct_field():
    ec, out, err = run_and_capture(
        'struct User { name: string, age: int }\n'
        'let u = User { "osman", 30 };\n'
        "print u.name;\n"
        "print len(u.name);\n"
        "print u.name + \"!\";\n"
        "print u;\n"
    )
    assert ec == 0, err
    assert out == ["osman", "5", "osman!", "{osman, 30}"]


def test_string_array_rejected():
    code, err = compile_only('let a: string[3];\n')
    assert code != 0


def test_new_string_array_rejected():
    code, err = compile_only('let a = new string[2];\n')
    assert code != 0
    assert "not supported" in err


def test_string_add_non_string_rejected():
    code, err = compile_only('let s = "a" + 1;\n')
    assert code != 0
    assert "string" in err


def test_string_compare_non_string_rejected():
    code, err = compile_only('let s = "a" == 1;\n')
    assert code != 0
    assert "string" in err


def test_string_relational_compare_rejected():
    code, err = compile_only('print "a" < "b";\n')
    assert code != 0
    assert "only == and !=" in err


def test_open_array_struct_field():
    ec, out, err = run_and_capture(
        "struct Bag { items: int[], tag: string }\n"
        'let b = Bag { new int[3], "box" };\n'
        "b.items[0] = 10; b.items[1] = 20; b.items[2] = 30;\n"
        "print len(b.items);\n"
        "print b.items[0] + b.items[1] + b.items[2];\n"
        "print b.tag;\n"
        "print b;\n"
    )
    assert ec == 0, err
    assert out == ["3", "60", "box", "{[10, 20, 30], box}"]


def test_open_array_struct_field_write_and_reassign():
    ec, out, err = run_and_capture(
        "struct Bag { items: int[] }\n"
        "let b = Bag { new int[3] };\n"
        "b.items[1] = 55;\n"
        "print b.items[1];\n"
        "b.items = new int[2];\n"
        "b.items[0] = 7;\n"
        "print len(b.items);\n"
        "print b.items[0];\n"
    )
    assert ec == 0, err
    assert out == ["55", "2", "7"]


def test_string_field_index_char():
    ec, out, err = run_and_capture(
        'struct Tag { name: string }\n'
        'let t = Tag { "abc" };\n'
        "print t.name[1];\n"
    )
    assert ec == 0, err
    assert out == ["b"]


def test_slice_field_from_array_literal():
    ec, out, err = run_and_capture(
        "struct Bag { items: int[] }\n"
        "let b = Bag { [10, 20, 30] };\n"
        "print len(b.items);\n"
        "print b.items[0] + b.items[1] + b.items[2];\n"
        "b.items[1] = 99;\n"
        "print b.items[1];\n"
        "print b;\n"
    )
    assert ec == 0, err
    assert out == ["3", "60", "99", "{[10, 99, 30]}"]


def test_slice_field_from_inferred_let():
    ec, out, err = run_and_capture(
        "struct Bag { items: int[], tag: string }\n"
        "let arr = [4, 5, 6];\n"
        "let b = Bag { arr, \"box\" };\n"
        "print len(b.items);\n"
        "print b.items[2];\n"
        "print b.tag;\n"
    )
    assert ec == 0, err
    assert out == ["3", "6", "box"]


def test_slice_field_assign_from_array_literal():
    ec, out, err = run_and_capture(
        "struct Bag { items: int[] }\n"
        "let b = Bag { new int[1] };\n"
        "b.items = [7, 8, 9];\n"
        "print len(b.items);\n"
        "print b.items[0] + b.items[1] + b.items[2];\n"
    )
    assert ec == 0, err
    assert out == ["3", "24"]


def test_slice_var_reassign_from_array_literal():
    ec, out, err = run_and_capture(
        "let a: int[] = [1, 2, 3];\n"
        "a = [9, 9];\n"
        "print len(a);\n"
        "print a[0] + a[1];\n"
    )
    assert ec == 0, err
    assert out == ["2", "18"]


def test_slice_field_from_struct_return():
    ec, out, err = run_and_capture(
        "struct Bag { items: int[] }\n"
        "func make() : Bag {\n"
        "    return Bag { [1, 2, 3] };\n"
        "}\n"
        "let b = make();\n"
        "print b.items[0] + b.items[2];\n"
    )
    assert ec == 0, err
    assert out == ["4"]


def test_array_of_structs_with_slice_field_literals():
    ec, out, err = run_and_capture(
        "struct Bag { items: int[] }\n"
        "let bs = [Bag { [1, 2] }, Bag { [3, 4, 5] }];\n"
        "print bs[0].items[1];\n"
        "print bs[1].items[2];\n"
    )
    assert ec == 0, err
    assert out == ["2", "5"]


def test_slice_field_type_mismatch_rejected():
    ec, err = compile_only(
        "struct Bag { items: int[] }\n"
        "let b = Bag { [1.5, 2.5] };\n"
    )
    assert ec != 0, "array of wrong element type should be rejected"
    assert "Type mismatch" in err


def test_slice_field_assign_type_mismatch_rejected():
    ec, err = compile_only(
        "struct Bag { items: int[] }\n"
        "let b = Bag { new int[1] };\n"
        "b.items = \"nope\";\n"
    )
    assert ec != 0, "non-array value must not be coercible to a slice field"
    assert "Type mismatch" in err


# --- epochs ---

def test_epoch_rollback_reuses_memory():
    ec, out, err = run_and_capture(
        "epoch {\n"
        "    let a = new int[1];\n"
        "    a[0] = 99;\n"
        "}\n"
        "let b = new int[1];\n"
        "print b[0];\n"
    )
    assert ec == 0, err
    assert out == ["0"], "arena should have rolled back so b reuses the epoch's slot and is re-zeroed"


def test_epoch_inner_allocations_survive_restore():
    ec, out, err = run_and_capture(
        "let keep = new int[2];\n"
        "keep[0] = 7; keep[1] = 8;\n"
        "epoch {\n"
        "    let tmp = new int[2];\n"
        "    tmp[0] = 1; tmp[1] = 2;\n"
        "}\n"
        "print keep[0] + keep[1];\n"
    )
    assert ec == 0, err
    assert out == ["15"]


def test_epoch_escape_assignment_rejected():
    ec, err = compile_only(
        "let outer: int[] = new int[2];\n"
        "epoch {\n"
        "    outer = new int[3];\n"
        "}\n"
    )
    assert ec != 0, "assigning a slice out of an epoch should be rejected"
    assert "epoch" in err.lower()


def test_epoch_return_rejected():
    ec, err = compile_only(
        "func f() : int {\n"
        "    epoch {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
    )
    assert ec != 0, "returning from inside an epoch should be rejected"
    assert "epoch" in err.lower()


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


def test_open_array_let_returned_is_arena_backed():
    ec, out, err = run_and_capture(
        "func make() : int[] {\n"
        "    let arr: int[] = [1, 2, 3];\n"
        "    return arr;\n"
        "}\n"
        "let s = make();\n"
        "print s;\n"
        "s[1] = 9;\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["[1, 2, 3]", "[1, 9, 3]"]


def test_arena_grows_past_64_mib():
    ec, out, err = run_and_capture(
        "let big: int[] = new int[17000000];\n"
        "big[16999999] = 42;\n"
        "print big[16999999];\n"
    )
    assert ec == 0, err
    assert out == ["42"]


def test_closed_arg_to_open_param_is_arena_backed():
    ec, out, err = run_and_capture(
        "func id(s: int[]) : int[] {\n"
        "    return s;\n"
        "}\n"
        "func maker() : int[] {\n"
        "    let r = id([1, 2, 3]);\n"
        "    return r;\n"
        "}\n"
        "let top = maker();\n"
        "print top;\n"
        "top[1] = 99;\n"
        "print top;\n"
    )
    assert ec == 0, err
    assert out == ["[1, 2, 3]", "[1, 99, 3]"]

