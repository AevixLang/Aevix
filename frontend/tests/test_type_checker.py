"""
Backend type-checker integration tests.

These run the full pipeline: frontend (.aev → ast.json) then backend (ast.json → exit code + stderr).
A correctly functioning type checker should reject invalid programs with exit code 1 and a clear
error message on stderr.
"""
import subprocess
import tempfile
import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
BACKEND_BIN = os.path.join(ROOT, "backend", "build", "aevix-backend")


def run_full_pipeline(code: str):
    """Run the full pipeline on `code`. Returns (exit_code, stderr_text)."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    ast_json = os.path.join(FRONTEND_DIR, "ast.json")
    try:
        # Frontend
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src],
            cwd=FRONTEND_DIR,
            capture_output=True,
            text=True,
        )
        if fe.returncode != 0:
            return fe.returncode, fe.stderr
        # Backend
        be = subprocess.run(
            [BACKEND_BIN, ast_json],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        return be.returncode, be.stderr
    finally:
        os.unlink(src)


def test_bool_plus_int_rejected():
    code = "let a: bool = true;\nlet b: int = a + 1;\n"
    code, err = run_full_pipeline(code)
    assert code != 0, "bool + int should be rejected"
    assert "+" in err


def test_string_plus_int_rejected():
    code = 'let x: string = "hi";\nlet y: int = x + 1;\n'
    code, err = run_full_pipeline(code)
    assert code != 0, "string + int should be rejected"
    assert "+" in err


def test_string_minus_rejected():
    code = 'let a: string = "a";\nlet b: string = "b";\nlet c = a - b;\n'
    code, err = run_full_pipeline(code)
    assert code != 0, "string - string should be rejected"


def test_if_non_bool_rejected():
    code = "if (5 + 3) { print 1; }\n"
    code, err = run_full_pipeline(code)
    assert code != 0, "if (int) should be rejected"
    assert "bool" in err.lower()


def test_while_non_bool_rejected():
    code = 'let x = 5;\nwhile (x) { x = 0; }\n'
    code, err = run_full_pipeline(code)
    assert code != 0, "while (int) should be rejected"
    assert "bool" in err.lower()


def test_not_non_bool_rejected():
    code = "let x = !(5 + 3);\n"
    code, err = run_full_pipeline(code)
    assert code != 0, "!(int) should be rejected"
    assert "!" in err


def test_and_non_bool_rejected():
    code = "let x = 1 && 2;\n"
    code, err = run_full_pipeline(code)
    assert code != 0, "int && int should be rejected"


def test_or_non_bool_rejected():
    code = "let x = 1 || 2;\n"
    code, err = run_full_pipeline(code)
    assert code != 0, "int || int should be rejected"


def test_bool_cmp_int_rejected():
    code = "let a: bool = true;\nlet b: int = 5;\nlet c = a == b;\n"
    code, err = run_full_pipeline(code)
    assert code != 0, "bool == int should be rejected"
    assert "Comparison" in err


def test_string_cmp_int_rejected():
    code = 'let x = "hello" == 5;\n'
    code, err = run_full_pipeline(code)
    assert code != 0, "string == int should be rejected"
    assert "Comparison" in err


def test_let_type_mismatch_string_to_int_rejected():
    code = 'let x: int = "hello";\n'
    code, err = run_full_pipeline(code)
    assert code != 0, 'let x: int = "hello" should be rejected'
    assert "string" in err


def test_let_type_mismatch_bool_to_int_rejected():
    code = "let x: int = true;\n"
    code, err = run_full_pipeline(code)
    assert code != 0, "let x: int = bool should be rejected"
    assert "mismatch" in err.lower() or "bool" in err


def test_assign_type_mismatch_int_to_string_rejected():
    code = 'let x: string = "hi";\nx = 5;\n'
    code, err = run_full_pipeline(code)
    assert code != 0, "assign int to string variable should be rejected"
    assert "mismatch" in err.lower()


def test_arg_type_mismatch_rejected():
    code = 'func take_float(x: float) { print x; }\ntake_float("oops");\n'
    code, err = run_full_pipeline(code)
    assert code != 0, "passing string to float param should be rejected"
    assert "Argument" in err


# Positive cases: valid programs must compile successfully

def test_int_plus_float_ok():
    code = "let x: float = 5 + 3.14;\nprint x;\n"
    code, err = run_full_pipeline(code)
    assert code == 0, f"int + float should be accepted, got stderr: {err}"


def test_let_widening_int_to_float_ok():
    code = "let x: float = 42;\nprint x;\n"
    code, err = run_full_pipeline(code)
    assert code == 0, f"let x: float = int should be accepted, got stderr: {err}"


def test_bool_ops_ok():
    code = "let a: bool = true && false || !false;\nprint a;\n"
    code, err = run_full_pipeline(code)
    assert code == 0, f"bool ops should be accepted, got stderr: {err}"


def test_int_comparison_ok():
    code = "if (1 < 2) { print 1; }\n"
    code, err = run_full_pipeline(code)
    assert code == 0, f"int comparison should be accepted, got stderr: {err}"


def test_bool_comparison_ok():
    code = 'let a = true == false;\nif (a) { print 1; }\n'
    code, err = run_full_pipeline(code)
    assert code == 0, f"bool == bool should be accepted, got stderr: {err}"
