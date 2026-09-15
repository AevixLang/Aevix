"""
Sema stage contract tests (Phase 1.2/1.3).

The sema stage is the single source of truth for static checks: types,
scopes, calls and the memory model's epoch-escape rules. These tests drive
the real pipeline up to `aevix-sema` and assert that:

  - every positive fixture parses AND passes sema (exit 0);
  - every negative fixture parses (the frontend does not own these
    diagnstics) but is rejected by sema (exit != 0) with a line/col
    diagnostic.

Phase 1.3 closes the documented memory-model gap "no-escape-4" (writing an
epoch slice into an outer struct field).
"""
import os
import subprocess
import tempfile

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
VENV_PYTHON = os.path.join(ROOT, "venv", "bin", "python")
FRONTEND_DIR = os.path.join(ROOT, "frontend")
SEMA_BIN = os.path.join(ROOT, "backend", "build", "aevix-sema")


def parse_and_run_stage(code, stage_bin):
    """Parse `code` with the frontend, then run `stage_bin` on its ast.json.

    Returns (frontend_rc, stage_rc, stage_stderr).
    """
    with tempfile.TemporaryDirectory(dir=FRONTEND_DIR) as td:
        src = os.path.join(td, "prog.aev")
        with open(src, "w") as f:
            f.write(code)
        ast_json = os.path.join(td, "ast.json")
        fe = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src, "-o", ast_json],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        if fe.returncode != 0:
            return fe.returncode, None, fe.stderr
        stage = subprocess.run([stage_bin, ast_json], cwd=ROOT,
                               capture_output=True, text=True)
        return fe.returncode, stage.returncode, stage.stderr


def sema_rejected(code):
    """Return the sema diagnostics; assert the fixture parses but fails sema."""
    fe_rc, sema_rc, err = parse_and_run_stage(code, SEMA_BIN)
    assert fe_rc == 0, f"fixture must parse, got rc={fe_rc}\n{err}\n{code}"
    assert sema_rc != 0, f"expected sema to REJECT:\n{code}\nsema rc={sema_rc}"
    return err


def sema_accepted(code):
    """Assert the fixture parses and passes sema."""
    fe_rc, sema_rc, err = parse_and_run_stage(code, SEMA_BIN)
    assert fe_rc == 0, f"fixture must parse, got rc={fe_rc}\n{err}\n{code}"
    assert sema_rc == 0, f"expected sema to ACCEPT:\n{code}\nsema rc={sema_rc} err={err}"


# ---------------------------------------------------------------------------
# 1.2 — scopes, calls, types, structs
# ---------------------------------------------------------------------------

def test_sema_rejects_undefined_variable():
    err = sema_rejected("func main() { print x; }")
    assert "undefined variable" in err


def test_sema_rejects_undefined_function():
    err = sema_rejected("func main() { foo(); }")
    assert "undefined function" in err


def test_sema_rejects_wrong_arg_count():
    err = sema_rejected("func add(a: int, b: int): int { return a + b; }\n"
                        "hot { let r = add(1); }")
    assert "wrong number of arguments" in err


def test_sema_rejects_arg_type_mismatch():
    err = sema_rejected("func add(a: int): int { return a; }\n"
                        "hot { let r = add(true); }")
    assert "argument type mismatch" in err


def test_sema_rejects_assignment_type_mismatch():
    err = sema_rejected("func main() { let a: int = 5; a = true; }")
    assert "incompatible type" in err


def test_sema_rejects_unknown_struct_member():
    err = sema_rejected("struct P { x: int }\n"
                        "func main() { let p = P { 1 }; print p.z; }")
    assert "unknown member" in err


def test_sema_rejects_struct_literal_arg_count():
    err = sema_rejected("struct P { x: int, y: int }\n"
                        "func main() { let p = P { 1 }; }")
    assert "wrong number of arguments" in err


def test_sema_rejects_return_type_mismatch():
    err = sema_rejected("func f(): int { return true; }")
    assert "return type" in err


def test_sema_rejects_redeclaration_same_scope():
    err = sema_rejected("func main() { let a = 1; let a = 2; }")
    assert "redeclaration" in err


def test_sema_rejects_indexing_non_array():
    err = sema_rejected("func main() { let a = 5; print a[0]; }")
    assert "non-array" in err


def test_sema_rejects_new_of_unknown_type():
    err = sema_rejected("func main() { let a = new Foo[3]; }")
    assert "unknown element type" in err


# ---------------------------------------------------------------------------
# 1.3 — epoch escape analysis
# ---------------------------------------------------------------------------

def test_sema_rejects_struct_field_epoch_escape():
    # Closes the documented memory-safety gap "no-escape-4": previously this
    # compiled and produced a use-after-free when the epoch rolled back.
    err = sema_rejected(
        "struct B { data: int[] }\n"
        "func main() {\n"
        "    let o = B { new int[1] };\n"
        "    epoch {\n"
        "        let inner = new int[3];\n"
        "        o.data = inner;\n"
        "    }\n"
        "    print o.data[0];\n"
        "}")
    assert "may be freed while referenced" in err


def test_sema_rejects_outer_var_epoch_escape():
    err = sema_rejected(
        "func main() {\n"
        "    let a: int[] = new int[1];\n"
        "    epoch {\n"
        "        a = new int[3];\n"
        "    }\n"
        "}")
    assert "may be freed while referenced" in err


def test_sema_rejects_returning_epoch_slice():
    err = sema_rejected("func f(): int[] {\n"
                        "    epoch { let s = new int[3]; return s; }\n"
                        "}")
    assert "allocated inside an epoch" in err


def test_sema_accepts_slice_confined_to_epoch():
    sema_accepted(
        "func main() {\n"
        "    epoch {\n"
        "        let inner = new int[3];\n"
        "        inner[0] = 7;\n"
        "        print inner[0];\n"
        "    }\n"
        "}")


def test_sema_accepts_epoch_slice_passed_to_inner_call():
    sema_accepted(
        "func sum(a: int[]): int { let s = 0; for i in a { s = s + i; } return s; }\n"
        "func main() {\n"
        "    epoch {\n"
        "        let inner = new int[3];\n"
        "        print sum(inner);\n"
        "    }\n"
        "}")


def test_sema_accepts_struct_epoch_local_usage():
    # A struct born inside the epoch may hold an epoch slice as long as it
    # never leaves the epoch.
    sema_accepted(
        "struct B { data: int[] }\n"
        "func main() {\n"
        "    epoch {\n"
        "        let inner = new int[3];\n"
        "        let box = B { inner };\n"
        "        print box.data[0];\n"
        "    }\n"
        "}")


def test_sema_accepts_primitive_epoch_locals():
    # Primitive values are copied, so they can never escape the arena.
    sema_accepted(
        "func main() {\n"
        "    let total = 0;\n"
        "    epoch {\n"
        "        let i = 42;\n"
        "        total = i;\n"
        "    }\n"
        "    print total;\n"
        "}")