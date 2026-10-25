"""
Match expression tests (v0.3.3).

A match expression yields a value, so it is exercised through the full
pipeline: parse, sema, codegen, run. Negative cases pin the type and
exhaustiveness rules that make the expression safe to lower to a PHI node.
"""
import os
import shutil
import subprocess
import tempfile

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
BACKEND_BIN = os.path.join(ROOT, "backend", "build", "aevix-backend")
SEMA_BIN = os.path.join(ROOT, "backend", "build", "aevix-sema")
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
            return fe.returncode, fe.stderr
        sm = subprocess.run(
            [SEMA_BIN, ast_json, src],
            cwd=ROOT, capture_output=True, text=True,
        )
        if sm.returncode != 0:
            return sm.returncode, sm.stderr
        be = subprocess.run(
            [BACKEND_BIN, ast_json],
            cwd=ROOT, capture_output=True, text=True,
        )
        return be.returncode, be.stderr
    finally:
        os.unlink(src)


def run_and_capture(code):
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
        run = subprocess.run([exe], capture_output=True, text=True,
                             cwd=workdir)
        return run.returncode, run.stdout.splitlines(), run.stderr
    finally:
        os.unlink(src)
        shutil.rmtree(workdir, ignore_errors=True)


def test_int_match_picks_case():
    ec, out, err = run_and_capture(
        "let x = 2;\n"
        "let y = match (x) { case 1 => 10, case 2 => 20, default => 30 };\n"
        "print y;\n"
    )
    assert ec == 0, err
    assert out == ["20"]


def test_int_match_falls_through_to_default():
    ec, out, err = run_and_capture(
        "let x = 9;\n"
        "let y = match (x) { case 1 => 10, case 2 => 20, default => 30 };\n"
        "print y;\n"
    )
    assert ec == 0, err
    assert out == ["30"]


def test_string_match():
    ec, out, err = run_and_capture(
        'let s = "b";\n'
        'let r = match (s) { case "a" => "alpha", case "b" => "beta", default => "?" };\n'
        "print r;\n"
    )
    assert ec == 0, err
    assert out == ["beta"]


def test_enum_match_exhaustive_without_default():
    ec, out, err = run_and_capture(
        "enum Color { Red, Green, Blue }\n"
        "let c = Color.Blue;\n"
        "let n = match (c) { case Color.Red => 1, case Color.Green => 2, case Color.Blue => 3 };\n"
        "print n;\n"
    )
    assert ec == 0, err
    assert out == ["3"]


def test_match_expression_feeds_call_and_condition():
    ec, out, err = run_and_capture(
        "func twice(x: int): int { return x * 2; }\n"
        "let n = 3;\n"
        "print twice(match (n) { case 3 => 21, default => 0 });\n"
        "if (match (n) { case 3 => true, default => false }) { print 1; }\n"
    )
    assert ec == 0, err
    assert out == ["42", "1"]


def test_nested_match_expression():
    ec, out, err = run_and_capture(
        "let a = 1;\n"
        "let b = 2;\n"
        "let r = match (a) {\n"
        "    case 1 => match (b) { case 2 => 12, default => 10 },\n"
        "    default => 0\n"
        "};\n"
        "print r;\n"
    )
    assert ec == 0, err
    assert out == ["12"]


def test_match_expression_in_loop():
    ec, out, err = run_and_capture(
        "let s = 0;\n"
        "for (let i = 0; i < 4; i += 1) {\n"
        "    s += match (i) { case 0 => 1, case 3 => 10, default => 100 };\n"
        "}\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["211"]


def test_non_exhaustive_enum_is_rejected():
    rc, err = compile_only(
        "enum Color { Red, Green, Blue }\n"
        "let c = Color.Red;\n"
        "let n = match (c) { case Color.Red => 1, case Color.Green => 2 };\n"
        "print n;\n"
    )
    assert rc != 0
    assert "non-exhaustive" in err


def test_missing_default_on_int_is_rejected():
    rc, err = compile_only(
        "let x = 1;\n"
        "let y = match (x) { case 1 => 10 };\n"
        "print y;\n"
    )
    assert rc != 0
    assert "default" in err


def test_mixed_arm_types_are_rejected():
    rc, err = compile_only(
        "let x = 1;\n"
        'let y = match (x) { case 1 => 10, default => "no" };\n'
        "print y;\n"
    )
    assert rc != 0
    assert "different types" in err


def test_case_value_type_mismatch_is_rejected():
    rc, err = compile_only(
        "let x = 1;\n"
        'let y = match (x) { case "one" => 10, default => 0 };\n'
        "print y;\n"
    )
    assert rc != 0
    assert "mismatch" in err
