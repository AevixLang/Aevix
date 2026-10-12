"""
Global variable contract tests.

A file-scope `let` must be readable and writable from every function body.
Codegen backs it with a module-level LLVM global rather than an alloca in
main, so a callee never references a value defined in another function.

Covers:
  - positive: scalar/string/slice globals mutated and read from functions,
    including inside recursion, and local shadowing of a global;
  - negative: an epoch slice assigned into a global and a type-mismatched
    assignment to a global are both rejected by sema.

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
SEMA_BIN = os.path.join(ROOT, "backend", "build", "aevix-sema")
LLC = shutil.which("llc") or "/opt/homebrew/opt/llvm/bin/llc"
OPT = shutil.which("opt") or "/opt/homebrew/opt/llvm/bin/opt"


def _parse(code):
    """Run the frontend on `code`; return (rc, ast_json, stderr)."""
    fd, src = tempfile.mkstemp(suffix=".aev", dir=FRONTEND_DIR)
    os.close(fd)
    ast_json = src + ".ast.json"
    try:
        with open(src, "w") as f:
            f.write(code)
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src, "-o", ast_json],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        return fe.returncode, ast_json, fe.stderr
    finally:
        if os.path.exists(src):
            os.unlink(src)


def sema_rejected(code):
    """Assert the fixture parses but is rejected by sema; return diagnostics."""
    fe_rc, ast_json, err = _parse(code)
    assert fe_rc == 0, f"fixture must parse, got rc={fe_rc}\n{err}\n{code}"
    stage = subprocess.run([SEMA_BIN, ast_json], cwd=ROOT,
                           capture_output=True, text=True)
    if os.path.exists(ast_json):
        os.unlink(ast_json)
    assert stage.returncode != 0, \
        f"expected sema to REJECT:\n{code}\nrc={stage.returncode}"
    return stage.stderr


def run_and_capture(code, args=(), stdin=None):
    """Compile and run, returning (exit_code, stdout_lines, error_text)."""
    fe_rc, ast_json, err = _parse(code)
    if fe_rc != 0:
        return fe_rc, [], err
    workdir = tempfile.mkdtemp()
    try:
        be = subprocess.run([BACKEND_BIN, ast_json], cwd=workdir,
                            capture_output=True, text=True)
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
        run = subprocess.run([exe, *args], capture_output=True, text=True,
                             cwd=workdir, input=stdin)
        return run.returncode, run.stdout.splitlines(), run.stderr
    finally:
        if os.path.exists(ast_json):
            os.unlink(ast_json)


# --- positive: scalar globals visible from function bodies ---

def test_global_scalar_written_from_function():
    ec, out, err = run_and_capture(
        "let counter = 0;\n"
        "func bump() { counter = counter + 1; }\n"
        "bump();\n"
        "print counter;\n"
    )
    assert ec == 0, err
    assert out == ["1"]


def test_global_accumulates_across_functions():
    ec, out, err = run_and_capture(
        "let counter = 0;\n"
        "let total = 100;\n"
        "func bump() { counter = counter + 1; }\n"
        "func bumpn(n: int) {\n"
        "    counter = counter + n;\n"
        "    total = total + n;\n"
        "}\n"
        "bump();\n"
        "bumpn(10);\n"
        "print counter;\n"
        "print total;\n"
    )
    assert ec == 0, err
    assert out == ["11", "110"]


def test_global_bool_and_float_from_function():
    ec, out, err = run_and_capture(
        "let flag = true;\n"
        "let ratio = 2.5;\n"
        "func flip() {\n"
        "    flag = false;\n"
        "    ratio = ratio * 2.0;\n"
        "}\n"
        "flip();\n"
        "print flag;\n"
        "print ratio;\n"
    )
    assert ec == 0, err
    assert out == ["false", "5.000000"]


def test_global_string_returned_from_function():
    ec, out, err = run_and_capture(
        "let name = \"hello\";\n"
        "func label(): string { return name; }\n"
        "print label();\n"
    )
    assert ec == 0, err
    assert out == ["hello"]


def test_global_visible_inside_recursion():
    ec, out, err = run_and_capture(
        "let shared = 7;\n"
        "let guard = 0;\n"
        "func recurse(n: int): int {\n"
        "    if (n <= 0) { return shared; }\n"
        "    shared = shared + 1;\n"
        "    guard = guard + 1;\n"
        "    return recurse(n - 1);\n"
        "}\n"
        "print recurse(5);\n"
        "print shared;\n"
        "print guard;\n"
    )
    assert ec == 0, err
    assert out == ["12", "12", "5"]


# --- positive: aggregate globals from function bodies ---

def test_global_slice_filled_from_functions():
    ec, out, err = run_and_capture(
        "let nums = new int[3];\n"
        "let acc = 0;\n"
        "func fill() {\n"
        "    nums[0] = 10;\n"
        "    nums[1] = 20;\n"
        "    nums[2] = 30;\n"
        "}\n"
        "func sum() { acc = nums[0] + nums[1] + nums[2]; }\n"
        "func run() { fill(); sum(); }\n"
        "run();\n"
        "print acc;\n"
        "print nums[2];\n"
    )
    assert ec == 0, err
    assert out == ["60", "30"]


def test_function_local_shadows_global():
    ec, out, err = run_and_capture(
        "let x = 1;\n"
        "func shadow() {\n"
        "    let x = 99;\n"
        "    print x;\n"
        "}\n"
        "shadow();\n"
        "print x;\n"
    )
    assert ec == 0, err
    assert out == ["99", "1"]


def test_top_level_only_program_unchanged():
    """A program with no functions must keep working after the codegen change."""
    ec, out, err = run_and_capture(
        "let a = 1;\n"
        "let b = 2;\n"
        "print a + b;\n"
    )
    assert ec == 0, err
    assert out == ["3"]


# --- negative: safety checks on global assignment stay in force ---

def test_sema_rejects_epoch_slice_into_global():
    err = sema_rejected(
        "let g: int[] = [];\n"
        "func stash() {\n"
        "    epoch {\n"
        "        g = new int[8];\n"
        "    }\n"
        "}\n"
        "stash();"
    )
    assert "epoch" in err or "freed" in err


def test_sema_rejects_type_mismatch_to_global_from_function():
    err = sema_rejected(
        "let g = 1;\n"
        "func bad() { g = \"text\"; }\n"
        "bad();"
    )
    assert "incompatible" in err or "type" in err.lower()
