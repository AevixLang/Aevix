"""
C++ frontend contract tests.

The Go CLI runs `backend/build/aevix-frontend`, while every other test in
this directory parses with the Python frontend. Whatever the C++ writer drops
from ast.json is therefore invisible to the rest of the suite — and the field
it used to drop was `line`/`col`, which leaves every diagnostic without a
source location.
"""
import json
import os
import subprocess
import tempfile

import pytest

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
CPP_FE = os.path.join(ROOT, "backend", "build", "aevix-frontend")
SEMA_BIN = os.path.join(ROOT, "backend", "build", "aevix-sema")
CLI = os.path.join(ROOT, "aevix")

pytestmark = pytest.mark.skipif(
    not os.path.exists(CPP_FE), reason="c++ frontend binary is not built"
)


def parse_with_cpp(code):
    """Runs the C++ frontend on `code` and returns the decoded ast.json."""
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "prog.aev")
        out = os.path.join(td, "ast.json")
        with open(src, "w") as f:
            f.write(code)
        proc = subprocess.run([CPP_FE, src, "-o", out], cwd=ROOT,
                              capture_output=True, text=True)
        assert proc.returncode == 0, f"frontend rejected a valid fixture:\n{proc.stderr}"
        with open(out) as f:
            return json.load(f)


def test_statement_positions_are_emitted():
    ast = parse_with_cpp("func f(): int { return 1; }\nprint f();\n")
    stmt = ast["body"][0]
    assert stmt["type"] == "FuncDecl"
    assert stmt["line"] == 1
    assert stmt["col"] == 1


def test_expression_positions_are_emitted():
    ast = parse_with_cpp("let x = 1;\nprint x;\n")
    value = ast["body"][0]["value"]
    assert value["type"] == "Number"
    assert value["line"] == 1
    assert value["col"] == 9


def test_nested_statements_keep_positions():
    ast = parse_with_cpp("func f(): int {\n    let x = 1;\n    return x;\n}\n")
    body = ast["body"][0]["body"]
    assert body[0]["line"] == 2 and body[0]["col"] == 5
    assert body[1]["line"] == 3 and body[1]["col"] == 5


@pytest.mark.skipif(not os.path.exists(SEMA_BIN), reason="sema is not built")
def test_diagnostic_carries_a_source_location():
    """End-to-end guard: ast.json positions must reach the printed diagnostic."""
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "prog.aev")
        out = os.path.join(td, "ast.json")
        with open(src, "w") as f:
            f.write("let a = 1;\nprint undefined_thing;\n")
        fe = subprocess.run([CPP_FE, src, "-o", out], cwd=ROOT,
                            capture_output=True, text=True)
        assert fe.returncode == 0, fe.stderr
        sema = subprocess.run([SEMA_BIN, out, src], cwd=ROOT,
                              capture_output=True, text=True)
        assert sema.returncode != 0, "fixture must fail sema"
        assert "undefined variable" in sema.stderr
        assert "--> " + src + ":2:7" in sema.stderr, sema.stderr


@pytest.mark.skipif(not os.path.exists(CLI), reason="aevix cli is not built")
def test_cli_reports_the_real_source_filename():
    """The driver must forward the source path, or diagnostics say input.aev."""
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "prog.aev")
        with open(src, "w") as f:
            f.write("let a = 1;\nprint undefined_thing;\n")
        subprocess.run([CLI, "build", src, "--stage", "parse"],
                       cwd=ROOT, capture_output=True, text=True)
        sema = subprocess.run([CLI, "build", src, "--stage", "sema"],
                              cwd=ROOT, capture_output=True, text=True)
        err = sema.stdout + sema.stderr
        assert sema.returncode != 0, err
        assert "undefined variable" in err
        assert "--> " + src + ":2:7" in err, err
        assert "input.aev" not in err, err
