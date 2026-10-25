"""
Permanent ban on nested function declarations (v0.3.4).

A `func` may only appear at the top level of a file. The compiler reports
`function declarations are only allowed at the top level` and does not
generate any code for the offending scope.
"""
import os
import subprocess
import tempfile

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
SEMA_BIN = os.path.join(ROOT, "backend", "build", "aevix-sema")


def _sema(code):
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    try:
        ast_json = os.path.join(FRONTEND_DIR, "ast.json")
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
        return sm.returncode, sm.stderr
    finally:
        os.unlink(src)


def test_nested_func_inside_func_rejected():
    rc, err = _sema("func outer() {\n    func inner() { }\n}\nmain();\n")
    assert rc != 0
    assert "only allowed at the top level" in err


def test_nested_func_inside_block_rejected():
    rc, err = _sema("hot {\n    func inner() { }\n}\nmain();\n")
    assert rc != 0
    assert "only allowed at the top level" in err
