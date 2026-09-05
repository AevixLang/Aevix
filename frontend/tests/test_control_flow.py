"""
Stage 4 feature integration tests: break/continue, compound assignment
(+= -= *= /=), multi-argument print, and bool output as true/false.

Runs the full pipeline and checks both compile acceptance/rejection and
runtime output, mirroring the harness in test_arrays.py.
"""
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


def run_and_capture(code, args=(), files=None, stdin=None, env=None):
    """Compile and run, returning (exit_code, stdout_lines, error_text)."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    workdir = tempfile.mkdtemp()
    pipe_env = dict(os.environ)
    if env:
        pipe_env.update(env)
    try:
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src],
            cwd=FRONTEND_DIR, capture_output=True, text=True, env=pipe_env,
        )
        if fe.returncode != 0:
            return fe.returncode, [], fe.stderr
        be = subprocess.run(
            [BACKEND_BIN, os.path.join(FRONTEND_DIR, "ast.json")],
            cwd=workdir, capture_output=True, text=True, env=pipe_env,
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
                             cwd=workdir, input=stdin, env=pipe_env)
        return run.returncode, run.stdout.splitlines(), run.stderr
    finally:
        os.unlink(src)


# --- break / continue ---

def test_break_exits_for():
    ec, out, err = run_and_capture(
        "let s = 0;\n"
        "for (let i = 0; i < 10; i += 1) {\n"
        "    if (i == 5) { break; }\n"
        "    s += i;\n"
        "}\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["10"]


def test_continue_skips_iteration():
    ec, out, err = run_and_capture(
        "let n = 0;\n"
        "for (let i = 0; i < 5; i += 1) {\n"
        "    if (i == 2) { continue; }\n"
        "    n += 1;\n"
        "}\n"
        "print n;\n"
    )
    assert ec == 0, err
    assert out == ["4"]


def test_break_while():
    ec, out, err = run_and_capture(
        "let i = 0;\n"
        "while (true) {\n"
        "    i += 1;\n"
        "    if (i == 3) { break; }\n"
        "}\n"
        "print i;\n"
    )
    assert ec == 0, err
    assert out == ["3"]


def test_continue_while_targets_condition():
    ec, out, err = run_and_capture(
        "let i = 0;\n"
        "while (i < 5) {\n"
        "    i += 1;\n"
        "    if (i == 2) { continue; }\n"
        "    print i;\n"
        "}\n"
        "print \"end\";\n"
    )
    assert ec == 0, err
    assert out == ["1", "3", "4", "5", "end"]


def test_break_continue_for_in():
    ec, out, err = run_and_capture(
        "let a = [1, 2, 3, 4];\n"
        "let s = 0;\n"
        "for x in a {\n"
        "    if (x == 2) { continue; }\n"
        "    if (x == 4) { break; }\n"
        "    s += x;\n"
        "}\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["4"]


def test_nested_loop_break_targets_inner():
    ec, out, err = run_and_capture(
        "let total = 0;\n"
        "for (let i = 0; i < 3; i += 1) {\n"
        "    for (let j = 0; j < 5; j += 1) {\n"
        "        if (j == 2) { break; }\n"
        "        total += i * 10 + j;\n"
        "    }\n"
        "}\n"
        "print total;\n"
    )
    assert ec == 0, err
    # i rows: (0+1) + (10+11) + (20+21) = 1 + 21 + 41
    assert out == ["63"]


def test_continue_in_for_step_updates_counter():
    ec, out, err = run_and_capture(
        "let n = 0;\n"
        "for (let i = 0; i < 4; i += 1) {\n"
        "    if (i == 1) { continue; }\n"
        "    n += i;\n"
        "}\n"
        "print n;\n"
    )
    assert ec == 0, err
    assert out == ["5"]


def test_break_outside_loop_rejected():
    code, err = compile_only("print 1;\nbreak;\n")
    assert code != 0
    assert "break" in err


def test_continue_outside_loop_rejected():
    code, err = compile_only("continue;\n")
    assert code != 0
    assert "continue" in err


# --- compound assignment ---

def test_compound_assignment_arith():
    ec, out, err = run_and_capture(
        "let x = 10;\n"
        "x += 5;\n"
        "x -= 3;\n"
        "x *= 4;\n"
        "x /= 2;\n"
        "print x;\n"
    )
    assert ec == 0, err
    assert out == ["24"]


def test_compound_float_target_int_rhs():
    ec, out, err = run_and_capture(
        "let f = 1.5;\n"
        "f += 2;\n"
        "f *= 3;\n"
        "print f;\n"
    )
    assert ec == 0, err
    assert out == ["10.500000"]


def test_compound_int_target_float_rhs_rejected():
    code, err = compile_only("let x = 5;\nx += 1.5;\n")
    assert code != 0


def test_compound_for_step():
    ec, out, err = run_and_capture(
        "let i = 0;\n"
        "let s = 0;\n"
        "for (; i < 5; i += 1) {\n"
        "    s += i * 2;\n"
        "}\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["20"]


def test_compound_array_element():
    ec, out, err = run_and_capture(
        "let a = [1, 2, 3];\n"
        "a[1] += 10;\n"
        "a[0] *= 5;\n"
        "a[2] -= 1;\n"
        "a[0] /= 2;\n"
        "print a;\n"
    )
    assert ec == 0, err
    assert out == ["[2, 12, 2]"]


def test_compound_float_array_element():
    ec, out, err = run_and_capture(
        "let a = [1.0, 2.0];\n"
        "a[0] += 0.5;\n"
        "a[1] *= 3.0;\n"
        "print a;\n"
    )
    assert ec == 0, err
    assert out == ["[1.500000, 6.000000]"]


def test_compound_struct_field():
    ec, out, err = run_and_capture(
        "struct P { x: int, y: float }\n"
        "let p = P { 2, 1.0 };\n"
        "p.x *= 3;\n"
        "p.x += 1;\n"
        "p.y += 0.5;\n"
        "print p.x, p.y;\n"
    )
    assert ec == 0, err
    assert out == ["7 1.500000"]


def test_compound_on_slice_rejected():
    code, err = compile_only(
        "let a = [1, 2];\nlet s = a;\ns += 1;\n"
    )
    assert code != 0


def test_compound_on_string_rejected():
    code, err = compile_only('let s = "hi";\ns += "!";\n')
    assert code != 0


def test_compound_on_bool_rejected():
    code, err = compile_only("let b = true;\nb += 1;\n")
    assert code != 0


# --- multi-argument print ---

def test_print_multiple_ints():
    ec, out, err = run_and_capture("print 1, 2, 3;\n")
    assert ec == 0, err
    assert out == ["1 2 3"]


def test_print_mixed_types():
    ec, out, err = run_and_capture(
        'print 1, "two", 3.5, true;\n'
    )
    assert ec == 0, err
    assert out == ["1 two 3.500000 true"]


def test_print_expr_list():
    ec, out, err = run_and_capture(
        "let a = 2;\nlet b = 3;\nprint a * b, a + b, a;\n"
    )
    assert ec == 0, err
    assert out == ["6 5 2"]


def test_print_single_still_newline():
    ec, out, err = run_and_capture("print 7;\nprint 8;\n")
    assert ec == 0, err
    assert out == ["7", "8"]


def test_print_array_then_int():
    ec, out, err = run_and_capture("let a = [1, 2];\nprint a, 5;\n")
    assert ec == 0, err
    assert out == ["[1, 2] 5"]


# --- bool printing ---

def test_print_bool_true_false():
    ec, out, err = run_and_capture(
        "func f(): bool { return true; }\n"
        "print f();\n"
        "print false;\n"
    )
    assert ec == 0, err
    assert out == ["true", "false"]


def test_print_bool_comparison():
    ec, out, err = run_and_capture("print 5 > 3;\nprint 5 < 3;\n")
    assert ec == 0, err
    assert out == ["true", "false"]


def test_print_bool_array():
    ec, out, err = run_and_capture("print [true, false, true];\n")
    assert ec == 0, err
    assert out == ["[true, false, true]"]