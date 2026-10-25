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


# ---------------------------------------------------------------------------
# package resolution (the C++ consumer half of the PM contract)
# ---------------------------------------------------------------------------

def _write_lock(proj, deps):
    """Writes an aevix.lock pinning `deps` (name -> (version, path))."""
    lock = {"name": "app", "version": "0.0.1",
            "dependencies": {n: {"version": v, "path": p} for n, (v, p) in deps.items()}}
    with open(os.path.join(proj, "aevix.lock"), "w") as f:
        json.dump(lock, f)


def _run_cpp_project(proj, main_src="main.aev"):
    """Runs the C++ frontend on proj/main.aev; returns (rc, stderr)."""
    out = os.path.join(proj, "ast.json")
    proc = subprocess.run([CPP_FE, os.path.join(proj, main_src), "-o", out],
                          cwd=proj, capture_output=True, text=True)
    return proc.returncode, proc.stderr


def test_cpp_package_resolution():
    with tempfile.TemporaryDirectory() as proj:
        pkg = os.path.join(proj, "calc@1.0.0")
        os.makedirs(pkg)
        with open(os.path.join(pkg, "num.aev"), "w") as f:
            f.write("func add(a: int, b: int): int { return a + b; }\n")
        _write_lock(proj, {"calc": ("1.0.0", "calc@1.0.0")})
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "calc:num";\nprint add(2, 3);\n')
        rc, err = _run_cpp_project(proj)
        assert rc == 0, err
        with open(os.path.join(proj, "ast.json")) as f:
            assert "add" in f.read()


def test_cpp_package_entry_point_lib_aev():
    with tempfile.TemporaryDirectory() as proj:
        pkg = os.path.join(proj, "tools@0.1.0")
        os.makedirs(pkg)
        with open(os.path.join(pkg, "lib.aev"), "w") as f:
            f.write("func tool_val(): int { return 55; }\n")
        _write_lock(proj, {"tools": ("0.1.0", "tools@0.1.0")})
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "tools:";\nprint tool_val();\n')
        rc, err = _run_cpp_project(proj)
        assert rc == 0, err
        with open(os.path.join(proj, "ast.json")) as f:
            assert "tool_val" in f.read()


def test_cpp_package_not_listed_in_lock():
    with tempfile.TemporaryDirectory() as proj:
        _write_lock(proj, {})
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "calc:num";\nprint 1;\n')
        rc, err = _run_cpp_project(proj)
        assert rc != 0
        assert "not in aevix.lock" in err
        assert "aevix-pm add calc" in err


def test_cpp_package_without_lock():
    with tempfile.TemporaryDirectory() as proj:
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "calc:num";\nprint 1;\n')
        rc, err = _run_cpp_project(proj)
        assert rc != 0
        assert "aevix.lock" in err


def test_cpp_package_module_not_found():
    with tempfile.TemporaryDirectory() as proj:
        pkg = os.path.join(proj, "calc@1.0.0")
        os.makedirs(pkg)
        with open(os.path.join(pkg, "num.aev"), "w") as f:
            f.write("func add(a: int, b: int): int { return a + b; }\n")
        _write_lock(proj, {"calc": ("1.0.0", "calc@1.0.0")})
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "calc:missing";\nprint 1;\n')
        rc, err = _run_cpp_project(proj)
        assert rc != 0
        assert "not found in package 'calc'" in err


# ---------------------------------------------------------------------------
# import aliases / namespaces
# ---------------------------------------------------------------------------

def test_cpp_import_alias_prefixes_declarations():
    with tempfile.TemporaryDirectory() as proj:
        with open(os.path.join(proj, "a.aev"), "w") as f:
            f.write("struct Entity { id: int }\nfunc make(id: int): Entity { return Entity { id: id }; }\n")
        with open(os.path.join(proj, "b.aev"), "w") as f:
            f.write("struct Entity { name: string }\nfunc make(name: string): Entity { return Entity { name: name }; }\n")
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write(
                'import "a.aev" as a;\n'
                'import "b.aev" as b;\n'
                'func main() {\n'
                '    let x: a.Entity = a.make(1);\n'
                '    let y: b.Entity = b.make("z");\n'
                '    print(x.id);\n'
                '    print(y.name);\n'
                '}\n'
                'main();\n'
            )
        rc, err = _run_cpp_project(proj)
        assert rc == 0, err
        with open(os.path.join(proj, "ast.json")) as f:
            ast = json.load(f)
        names = {s.get("name") for s in ast["body"] if "name" in s}
        assert {"a__Entity", "a__make", "b__Entity", "b__make"} <= names
        assert "main" in names

        main_decl = [s for s in ast["body"] if s.get("name") == "main"][0]
        lets = [s for s in main_decl["body"] if s["type"] == "Let"]
        assert lets[0]["var_type"] == "a__Entity"
        assert lets[0]["value"]["callee"] == "a__make"
        assert lets[1]["value"]["callee"] == "b__make"


def test_cpp_import_alias_qualified_struct_literal():
    with tempfile.TemporaryDirectory() as proj:
        with open(os.path.join(proj, "shapes.aev"), "w") as f:
            f.write("struct Point { x: int, y: int }\n")
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write(
                'import "shapes.aev" as s;\n'
                'let p: s.Point = s.Point { x: 1, y: 2 };\n'
                'print(p.x);\n'
            )
        rc, err = _run_cpp_project(proj)
        assert rc == 0, err
        with open(os.path.join(proj, "ast.json")) as f:
            ast = json.load(f)
        let = [s for s in ast["body"] if s["type"] == "Let"][0]
        assert let["var_type"] == "s__Point"
        assert let["value"]["type"] == "StructLiteral"
        assert let["value"]["name"] == "s__Point"
