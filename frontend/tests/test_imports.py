"""
Stage 3 integration tests: import / modules.

Covers path imports, transitive imports, dedup (diamond + same file via
different specs), dropping of module top-level side effects, cycle detection,
missing modules, name conflicts across files, rejection of nested imports,
the reserved std: prefix, package resolution via aevix.lock, project lib/,
and AEVIX_PATH. Full pipeline except the explicitly frontend-only cases.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from src.parser import parse

from test_arrays import (
    run_and_capture, FRONTEND_DIR, ROOT, VENV_PYTHON,
    BACKEND_BIN, RUNTIME_OBJ, LLC,
)

FIX = "tests/fixtures"


def fe_errors(code):
    """Run only the frontend; return (rc, stderr)."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(code)
        f.flush()
        src = f.name
    try:
        p = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", src],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        return p.returncode, p.stderr
    finally:
        os.unlink(src)


def full_program(project_dir, main_name="main.aev"):
    """Run the whole pipeline inside project_dir; return stdout lines.
    The frontend runs from FRONTEND_DIR (so `src.main` is importable) but
    imports still resolve relative to project_dir, keeping the project's
    aevix.lock in play."""
    fe = subprocess.run(
        [VENV_PYTHON, "-m", "src.main", os.path.join(project_dir, main_name)],
        cwd=FRONTEND_DIR, capture_output=True, text=True,
    )
    assert fe.returncode == 0, fe.stderr
    ast_json = os.path.join(FRONTEND_DIR, "ast.json")
    be = subprocess.run(
        [BACKEND_BIN, ast_json],
        cwd=project_dir, capture_output=True, text=True,
    )
    assert be.returncode == 0, be.stderr
    ir = os.path.join(project_dir, "out.ll")
    with open(ir, "w") as fh:
        fh.write(be.stdout)
    obj = os.path.join(project_dir, "out.o")
    lc = subprocess.run([LLC, "-filetype=obj", ir, "-o", obj],
                        capture_output=True, text=True)
    assert lc.returncode == 0, lc.stderr
    exe = os.path.join(project_dir, "prog")
    cl = subprocess.run(["clang", obj, RUNTIME_OBJ, "-o", exe],
                        capture_output=True, text=True)
    assert cl.returncode == 0, cl.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    return r.stdout.splitlines()


# ---------------------------------------------------------------------------
# path imports (committed fixtures under frontend/tests/fixtures)
# ---------------------------------------------------------------------------

def test_import_basic_transitive():
    code = (
        f'import "{FIX}/geo_ext";\n'
        "let v = cube_volume(3);\n"
        "print v;\n"
        "print square_area(4);\n"
        "print circle_area(2.0);\n"
    )
    ec, out, err = run_and_capture(code)
    assert ec == 0, err
    assert out == ["27", "16", "12.560000"]


def test_import_direct_no_extension():
    ec, out, err = run_and_capture(
        f'import "{FIX}/geo";\n'
        "print square_area(5);\n"
    )
    assert ec == 0, err
    assert out == ["25"]


def test_import_statement_dropped_from_ast_json():
    """Imports disappear from the emitted Program; only decls + exec remain."""
    from src.ast import to_dict
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".aev", delete=False, dir=FRONTEND_DIR
    ) as f:
        f.write(f'import "{FIX}/geo";\nprint square_area(2);\n')
        f.flush()
        src = f.name
    try:
        p = subprocess.run([VENV_PYTHON, "-m", "src.main", src],
                           cwd=FRONTEND_DIR, capture_output=True, text=True)
        assert p.returncode == 0, p.stderr
        with open(os.path.join(FRONTEND_DIR, "ast.json")) as fh:
            body = json.load(fh)["body"]
    finally:
        os.unlink(src)
    assert all(s["type"] != "Import" for s in body)
    assert {"square_area", "circle_area"} <= {
        s.get("name") for s in body if s.get("name")
    }


def test_dedup_diamond():
    code = (
        f'import "{FIX}/uses_shared_a";\n'
        f'import "{FIX}/uses_shared_b";\n'
        "print via_a(3);\n"
        "print via_b(3);\n"
        "print twice(10);\n"
    )
    ec, out, err = run_and_capture(code)
    assert ec == 0, err
    assert out == ["7", "8", "20"]


def test_dedup_same_file_different_spec():
    code = (
        f'import "{FIX}/geo";\n'
        f'import "{FIX}/geo.aev";\n'
        "print square_area(3);\n"
    )
    ec, out, err = run_and_capture(code)
    assert ec == 0, err
    assert out == ["9"]


def test_module_side_effects_dropped():
    ec, out, err = run_and_capture(
        f'import "{FIX}/module_side_effect";\n'
        "print quiet();\n"
    )
    assert ec == 0, err
    assert out == ["7"]


# ---------------------------------------------------------------------------
# resolution errors
# ---------------------------------------------------------------------------

def test_missing_module():
    rc, err = fe_errors(f'import "tests/fixtures/does_not_exist";\nprint 1;\n')
    assert rc != 0
    assert "not found" in err
    assert "searched" in err


def test_circular_import():
    rc, err = fe_errors(f'import "{FIX}/cyc_a";\nprint 1;\n')
    assert rc != 0
    assert "circular import" in err


def test_self_import():
    path = os.path.join(FRONTEND_DIR, "__aevix_self_imp.aev")
    with open(path, "w") as f:
        f.write('import "__aevix_self_imp";\nprint 1;\n')
    try:
        p = subprocess.run([VENV_PYTHON, "-m", "src.main", path],
                           cwd=FRONTEND_DIR, capture_output=True, text=True)
        assert p.returncode != 0
        assert "circular import" in p.stderr
    finally:
        os.unlink(path)


def test_conflict_across_modules():
    rc, err = fe_errors(
        f'import "{FIX}/geo";\n'
        f'import "{FIX}/dup";\n'
        "print 1;\n"
    )
    assert rc != 0
    assert "name conflict" in err
    assert "geo.aev" in err and "dup.aev" in err


def test_conflict_with_main():
    rc, err = fe_errors(
        "func square_area(side: int): int { return side; }\n"
        f'import "{FIX}/geo";\n'
        "print 1;\n"
    )
    assert rc != 0
    assert "name conflict" in err


def test_nested_import_rejected():
    rc, err = fe_errors(
        f'func f() {{ import "{FIX}/geo"; return 1; }}\nprint 1;\n'
    )
    assert rc != 0
    assert "top level" in err


def test_std_reserved():
    rc, err = fe_errors('import "std:io";\nprint 1;\n')
    assert rc != 0
    assert "reserved" in err


def test_package_without_lock():
    rc, err = fe_errors('import "calc:num";\nprint 1;\n')
    assert rc != 0
    assert "aevix.lock" in err


def test_import_is_keyword():
    rc, err = fe_errors("let import = 5;\nprint import;\n")
    assert rc != 0


# ---------------------------------------------------------------------------
# package resolution via aevix.lock (the compiler-side half of the PM contract)
# ---------------------------------------------------------------------------

def test_package_resolution():
    with tempfile.TemporaryDirectory() as proj:
        pkg = os.path.join(proj, "calc@1.0.0")
        os.makedirs(pkg)
        with open(os.path.join(pkg, "num.aev"), "w") as f:
            f.write("func add(a: int, b: int): int { return a + b; }\n")
        lock = {
            "name": "app",
            "version": "0.0.1",
            "dependencies": {"calc": {"version": "1.0.0", "path": pkg}},
        }
        with open(os.path.join(proj, "aevix.lock"), "w") as f:
            json.dump(lock, f)
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "calc:num";\nprint add(2, 3);\n')
        assert full_program(proj) == ["5"]


def test_package_not_listed_in_lock():
    with tempfile.TemporaryDirectory() as proj:
        with open(os.path.join(proj, "aevix.lock"), "w") as f:
            json.dump({"name": "app", "dependencies": {}}, f)
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "calc:num";\nprint 1;\n')
        p = subprocess.run(
            [VENV_PYTHON, "-m", "src.main", os.path.join(proj, "main.aev")],
            cwd=FRONTEND_DIR, capture_output=True, text=True,
        )
        assert p.returncode != 0
        assert "not in aevix.lock" in p.stderr
        assert "aevix-pm add calc" in p.stderr


def test_package_entry_point_lib_aev():
    """A package dir itself (no module path) resolves to its lib.aev."""
    with tempfile.TemporaryDirectory() as proj:
        pkg = os.path.join(proj, "tools@0.1.0")
        os.makedirs(pkg)
        with open(os.path.join(pkg, "lib.aev"), "w") as f:
            f.write("func tool_val(): int { return 55; }\n")
        lock = {
            "name": "app",
            "dependencies": {"tools": {"version": "0.1.0", "path": pkg}},
        }
        with open(os.path.join(proj, "aevix.lock"), "w") as f:
            json.dump(lock, f)
        with open(os.path.join(proj, "main.aev"), "w") as f:
            f.write('import "tools:";\nprint tool_val();\n')
        assert full_program(proj) == ["55"]


# ---------------------------------------------------------------------------
# additional search locations: project lib/ and AEVIX_PATH
# ---------------------------------------------------------------------------

def test_lib_dir_resolution():
    libdir = os.path.join(FRONTEND_DIR, "lib", "modlib")
    os.makedirs(libdir, exist_ok=True)
    try:
        with open(os.path.join(libdir, "helper.aev"), "w") as f:
            f.write("func helper_val(): int { return 99; }\n")
        ec, out, err = run_and_capture(
            'import "modlib/helper";\nprint helper_val();\n'
        )
        assert ec == 0, err
        assert out == ["99"]
    finally:
        shutil.rmtree(os.path.join(FRONTEND_DIR, "lib"))


def test_aeviX_path_resolution():
    with tempfile.TemporaryDirectory() as extra:
        os.makedirs(os.path.join(extra, "extramod"))
        with open(os.path.join(extra, "extramod", "helper.aev"), "w") as f:
            f.write("func ext_val(): int { return 123; }\n")
        ec, out, err = run_and_capture(
            'import "extramod/helper";\nprint ext_val();\n',
            env={"AEVIX_PATH": extra},
        )
        assert ec == 0, err
        assert out == ["123"]


# ---------------------------------------------------------------------------
# parser-level checks
# ---------------------------------------------------------------------------

def test_parser_import_node():
    prog = parse('import "abc.def";\nlet x = 1;\n')
    assert prog.body[0].type == "Import"
    assert prog.body[0].spec == "abc.def"
    assert prog.body[1].type == "Let"