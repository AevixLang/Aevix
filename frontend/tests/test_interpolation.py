"""
String interpolation (f-string) contract tests.

`f"...{expr}..."` is desugared at parse time into a `+` chain of string
literals and to_str() calls, in both the C++ and the Python frontend, so sema
and codegen need no interpolation-specific logic. These tests drive the C++
(primary) frontend through the full native pipeline and check the printed
output, then assert the Python fallback desugars to the same shape so the two
frontends never diverge.
"""
import os
import subprocess
import tempfile

import pytest

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
CPP_FE = os.path.join(ROOT, "backend", "build", "aevix-frontend")
BACKEND_BIN = os.path.join(ROOT, "backend", "build", "aevix-backend")
RUNTIME_OBJ = os.path.join(ROOT, "backend", "build", "runtime.o")
LLC = "/opt/homebrew/opt/llvm/bin/llc"
OPT = "/opt/homebrew/opt/llvm/bin/opt"

pytestmark = pytest.mark.skipif(
    not os.path.exists(CPP_FE), reason="c++ frontend binary is not built"
)


def _run_pipeline(frontend_cmd, code):
    """Compiles `code` through frontend->backend->opt->llc->clang and runs it.

    `frontend_cmd` is the argv prefix that writes ast.json (path appended).
    Returns (exit_code, stdout_lines, stderr)."""
    with tempfile.TemporaryDirectory() as workdir:
        src = os.path.join(workdir, "prog.aev")
        with open(src, "w") as f:
            f.write(code)
        ast_json = os.path.join(workdir, "ast.json")

        fe = subprocess.run(frontend_cmd + [src, "-o", ast_json],
                            cwd=workdir, capture_output=True, text=True)
        if fe.returncode != 0:
            return fe.returncode, [], fe.stderr

        be = subprocess.run([BACKEND_BIN, ast_json],
                            cwd=workdir, capture_output=True, text=True)
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

        run = subprocess.run([exe], cwd=workdir, capture_output=True, text=True)
        return run.returncode, run.stdout.splitlines(), run.stderr


def run_cpp(code):
    """Runs a snippet through the C++ (primary) frontend pipeline."""
    return _run_pipeline([CPP_FE], code)


# --- runtime behaviour (C++ frontend) ---

def test_interpolates_string_int_float():
    ec, out, err = run_cpp(
        'let n = 5;\n'
        'let s = "world";\n'
        'let x = 2.5;\n'
        'print f"hi {s} n={n} x={x}";\n'
    )
    assert ec == 0, err
    assert out == ["hi world n=5 x=2.500000"]


def test_interpolates_expression():
    ec, out, err = run_cpp(
        "let a = 2;\n"
        "let b = 3;\n"
        'print f"sum {a + b}";\n'
    )
    assert ec == 0, err
    assert out == ["sum 5"]


def test_literal_braces_are_escaped():
    ec, out, err = run_cpp(
        'print f"use {{ }} to escape";\n'
    )
    assert ec == 0, err
    assert out == ["use { } to escape"]


def test_fstring_without_expression_is_plain_string():
    ec, out, err = run_cpp('print f"no expr here";\n')
    assert ec == 0, err
    assert out == ["no expr here"]


def test_adjacent_segments_concatenate():
    ec, out, err = run_cpp(
        'let a = "x";\n'
        'let b = "y";\n'
        'print f"{a}{b}";\n'
    )
    assert ec == 0, err
    assert out == ["xy"]


def test_f_remains_a_usable_identifier():
    # The `f` marker only fires on `f"`; a variable named f must still work.
    ec, out, err = run_cpp(
        "let f = 9;\n"
        "print f;\n"
        'print f"f={f}";\n'
    )
    assert ec == 0, err
    assert out == ["9", "f=9"]


def test_interpolation_inside_function():
    ec, out, err = run_cpp(
        'func greet(name: string) : string {\n'
        '    return f"hello {name}";\n'
        "}\n"
        'print greet("aevix");\n'
    )
    assert ec == 0, err
    assert out == ["hello aevix"]


def test_unterminated_brace_is_a_parse_error():
    ec, out, err = run_cpp('print f"bad {x";\n')
    assert ec != 0


def test_empty_braces_are_a_parse_error():
    ec, out, err = run_cpp('print f"bad {}";\n')
    assert ec != 0


# --- Python fallback parity (parse-time desugaring) ---

def _desugar(code):
    """Parses `code` with the Python frontend and returns the first Print's
    expression AST node."""
    from frontend.src.parser import parse
    prog = parse(code)
    for stmt in prog.body:
        if getattr(stmt, "type", None) == "Print":
            return stmt.args[0]
    raise AssertionError("no print statement found")


def test_python_fallback_desugars_to_concat_of_to_str():
    node = _desugar('let a = 1;\nprint f"n={a}";\n')
    # Add( String("n="), Call(to_str, Variable a) )
    assert node.type == "Add"
    assert node.left.type == "String" and node.left.value == "n="
    call = node.right
    assert call.type == "Call" and call.callee == "to_str"
    assert call.args[0].type == "Variable" and call.args[0].value == "a"


def test_python_fallback_escapes_braces():
    node = _desugar('print f"a {{b}} c";\n')
    assert node.type == "String"
    assert node.value == "a {b} c"
