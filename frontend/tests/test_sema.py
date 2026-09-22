"""
Sema stage contract tests.

The sema stage is the single source of truth for static checks: types,
scopes, calls and the memory model's epoch-escape rules. These tests drive
the real pipeline up to `aevix-sema` and assert that:

  - every positive fixture parses AND passes sema (exit 0);
  - every negative fixture parses (the frontend does not own these
    diagnstics) but is rejected by sema (exit != 0) with a line/col
    diagnostic.

Covers the documented memory-model gap "no-escape-4": writing an
epoch slice into an outer struct field.
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


def test_sema_rejects_param_escape_through_function():
    err = sema_rejected(
        "let global_buf: int[] = [];\n"
        "func stash(data: int[]) {\n"
        "    global_buf = data;\n"
        "}\n"
        "func main() {\n"
        "    epoch {\n"
        "        let tmp = new int[50];\n"
        "        stash(tmp);\n"
        "    }\n"
        "}")
    assert "escape" in err.lower()


def test_sema_accepts_non_epoch_param_to_escaping_fn():
    sema_accepted(
        "let global_buf: int[] = [];\n"
        "func stash(data: int[]) {\n"
        "    global_buf = data;\n"
        "}\n"
        "func main() {\n"
        "    let local = new int[50];\n"
        "    stash(local);\n"
        "}")


def test_sema_rejects_returning_epoch_slice():
    err = sema_rejected("func f(): int[] {\n"
                        "    epoch { let s = new int[3]; return s; }\n"
                        "}")
    assert "arena-allocated" in err or "epoch" in err


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


# ---------------------------------------------------------------------------
# 1.5 — fixed-width types, conversions and enum (sema stage)
#
# The sema stage is the single repo for conversion diagnostics: only explicit
# to_* builtins may narrow, constants are range-checked at compile time, and
# enum values are first-class integers that must stay inside their type.
# ---------------------------------------------------------------------------

def test_sema_rejects_narrowing_let():
    err = sema_rejected("func main() { let a: i64 = 5; let b: i32 = a; }")
    assert "mismatch" in err


def test_sema_rejects_width_signed_mismatch_let():
    err = sema_rejected("func main() { let a: i32 = 5; let b: u8 = a; }")
    assert "mismatch" in err


def test_sema_rejects_float_to_int_let():
    err = sema_rejected("func main() { let x: float = 2.5; let y: int = x; }")
    assert "mismatch" in err


def test_sema_rejects_constant_overflow():
    err = sema_rejected("func main() { let x: i8 = 300; }")
    assert "out of range for i8" in err


def test_sema_rejects_unsigned_overflow():
    err = sema_rejected("func main() { let x: u8 = 300; }")
    assert "out of range for u8" in err


def test_sema_rejects_constant_underflow_unsigned():
    err = sema_rejected("func main() { let x: u8 = -1; }")
    assert "out of range for u8" in err


def test_sema_rejects_untyped_literal_overflow():
    # A bare literal over i32 max must name its wider type.
    err = sema_rejected("func main() { let x = 2147483648; }")
    assert "out of range for int" in err


def test_sema_rejects_conversion_constant_overflow():
    err = sema_rejected("func main() { let x: u8 = to_u8(300); }")
    assert "out of range for u8" in err


def test_sema_rejects_conversion_of_non_numeric():
    err = sema_rejected('func main() { let x = to_int("42"); }')
    assert "requires a numeric argument" in err


def test_sema_rejects_mixed_signedness_arith():
    err = sema_rejected("func main() { let a: i8 = 1; let b: u8 = 2; print a + b; }")
    assert "compatible type" in err


def test_sema_rejects_mixed_signedness_comparison():
    err = sema_rejected("func main() { let b: u8 = 1; if (b > 300) { print 1; } }")
    assert "comparison" in err


def test_sema_rejects_string_dot_less():
    err = sema_rejected('func main() { if ("a" < "b") { print 1; } }')
    assert "comparison" in err


def test_sema_accepts_widening_chain():
    sema_accepted("func main() { let a: i8 = 5; let b: i32 = a;"
                  " let f: float = b; print f; }")


def test_sema_accepts_constant_shrink():
    sema_accepted("func main() { let a: u8 = 200; let b: i8 = -128; print a; print b; }")


def test_sema_accepts_wide_unsigned_constants():
    sema_accepted("func main() { let a: u8 = 255; let b: u32 = 4294967295;"
                  " let c: u64 = 18446744073709551615; print a; }")


def test_sema_accepts_i64_max_and_min():
    sema_accepted("func main() { let max: i64 = 9223372036854775807;"
                  " let min: i64 = -9223372036854775808; print max; print min; }")


def test_sema_accepts_i32_min_literal():
    sema_accepted("func main() { let x = -2147483648; print x; }")


def test_sema_accepts_conversion_narrowing():
    sema_accepted("func main() { let a: i32 = 300; let b: u8 = to_u8(a); print b; }")


def test_sema_accepts_conversion_widening():
    sema_accepted("func main() { let a: i8 = 9; let b: i64 = to_i64(a);"
                  " let f: f32 = to_f32(b); print f; }")


def test_sema_accepts_float_broadening():
    sema_accepted("func main() { let a: f32 = 1.5; let b: f64 = a; print b; }")


def test_sema_accepts_comparison_with_fitting_constant():
    sema_accepted("func main() { let b: u8 = 200; if (b > 199) { print 1; } }")


def test_sema_rejects_redeclared_enum():
    err = sema_rejected("enum Color { red }\n"
                        "enum Color { green }\n"
                        "func main() { print 1; }")
    assert "redeclaration of enum" in err


def test_sema_rejects_duplicate_enum_variant():
    err = sema_rejected("enum Color { red, red }\nfunc main() { print 1; }")
    assert "duplicate enum variant" in err


def test_sema_rejects_enum_name_clash():
    err = sema_rejected("struct Color { r: int }\n"
                        "enum Color { red }\n"
                        "func main() { print 1; }")
    assert "conflicts" in err


def test_sema_rejects_unknown_enum_variant():
    err = sema_rejected("enum Color { red, green }\n"
                        "func main() { let c = Color.purple; }")
    assert "unknown enum variant" in err


def test_sema_rejects_int_to_enum_let():
    err = sema_rejected("enum Color { red }\n"
                        "func main() { let n: int = 1; let c: Color = n; }")
    assert "mismatch" in err


def test_sema_rejects_assign_to_enum_variant():
    err = sema_rejected("enum Color { red }\nfunc main() { Color.red = 1; }")
    assert "cannot assign to an enum variant" in err


def test_sema_rejects_ref_param_to_enum_variant():
    err = sema_rejected("enum Color { red }\n"
                        "func flip(c: ref Color) { c = Color.red; }\n"
                        "func main() { flip(Color.red); }")
    assert "ref parameter cannot bind an enum variant" in err


def test_sema_rejects_cross_enum_comparison():
    err = sema_rejected("enum A { x }\nenum B { y }\n"
                        "func main() { let r = A.x == B.y; print r; }")
    assert "same type" in err


def test_sema_accepts_enum_usage():
    sema_accepted(
        "enum Color { red, green, blue, }\n"
        "func describe(c: Color) { if (c == Color.red) { print 1; } }\n"
        "func main() { let c = Color.green; describe(c); }")


def test_sema_accepts_enum_array():
    sema_accepted(
        "enum Color { red, green }\n"
        "func first(cs: Color[]): Color { return cs[0]; }\n"
        "func main() {\n"
        "    let cs = new Color[2];\n"
        "    cs[0] = Color.green;\n"
        "    print cs[0];\n"
        "}")


def test_sema_accepts_ref_param_to_enum_variable():
    sema_accepted(
        "enum Color { red, green }\n"
        "func flip(c: ref Color) { c = Color.green; }\n"
        "func main() { let color = Color.red; flip(color); print 1; }")


def test_sema_accepts_enum_in_struct():
    sema_accepted(
        "enum Color { red, green }\n"
        "struct Pixel { c: Color, a: i64 }\n"
        "func main() { let p = Pixel { Color.red, 7 }; print p.a; }")


# ---------------------------------------------------------------------------
# 1.6 — break/continue must be inside a loop
# ---------------------------------------------------------------------------

def test_sema_rejects_break_outside_loop():
    err = sema_rejected("func main() { break; }")
    assert "break/continue outside of loop" in err


def test_sema_rejects_continue_outside_loop():
    err = sema_rejected("func main() { continue; }")
    assert "break/continue outside of loop" in err


def test_sema_rejects_break_in_if_outside_loop():
    err = sema_rejected("func main() { if (true) { break; } }")
    assert "break/continue outside of loop" in err


def test_sema_rejects_continue_in_epoch_outside_loop():
    err = sema_rejected("func main() { epoch { continue; } }")
    assert "break/continue outside of loop" in err


def test_sema_accepts_break_inside_for():
    sema_accepted("func main() { for (let i = 0; i < 10; i += 1) { if (i == 5) { break; } } }")


def test_sema_accepts_continue_inside_while():
    sema_accepted("func main() { let i = 0; while (i < 10) { i += 1; continue; } }")


def test_sema_accepts_break_inside_for_in():
    sema_accepted("func main() { let a = [1, 2, 3]; for x in a { if (x == 2) { break; } } }")


# ---------------------------------------------------------------------------
# 1.7 — builtin argument count and type checking
# ---------------------------------------------------------------------------

def test_sema_rejects_len_wrong_arg_count():
    err = sema_rejected("func main() { len(1, 2); }")
    assert "expects one argument" in err


def test_sema_rejects_rand_wrong_arg_count():
    err = sema_rejected("func main() { rand(); }")
    assert "expects one argument" in err


def test_sema_rejects_rand_non_int_arg():
    err = sema_rejected('func main() { rand("hello"); }')
    assert "expects an integer argument" in err


def test_sema_rejects_assert_wrong_arg_count():
    err = sema_rejected("func main() { assert(1, 2); }")
    assert "expects one argument" in err


def test_sema_rejects_assert_non_int_arg():
    err = sema_rejected('func main() { assert("yes"); }')
    assert "expects an integer argument" in err


def test_sema_rejects_trim_non_string():
    err = sema_rejected("func main() { trim(42); }")
    assert "expects a string argument" in err


def test_sema_rejects_upper_wrong_arg_count():
    err = sema_rejected("func main() { upper(); }")
    assert "expects one argument" in err


def test_sema_rejects_join_wrong_arg_count():
    err = sema_rejected("func main() { join(\"a\"); }")
    assert "expects two arguments" in err


def test_sema_rejects_replace_wrong_arg_count():
    err = sema_rejected('func main() { replace("a", "b"); }')
    assert "expects three arguments" in err


def test_sema_rejects_min_non_numeric():
    err = sema_rejected('func main() { min("a", "b"); }')
    assert "expects numeric arguments" in err


def test_sema_rejects_write_wrong_arg_count():
    err = sema_rejected('func main() { write("f"); }')
    assert "expects two arguments" in err


def test_sema_rejects_read_wrong_arg_count():
    err = sema_rejected('func main() { read("a", "b"); }')
    assert "expects one argument" in err


def test_sema_rejects_exit_wrong_arg_count():
    err = sema_rejected("func main() { exit(); }")
    assert "expects one argument" in err


def test_sema_rejects_exit_non_int():
    err = sema_rejected('func main() { exit("bye"); }')
    assert "expects an integer argument" in err


def test_sema_rejects_startswith_wrong_arg_count():
    err = sema_rejected('func main() { startswith("a"); }')
    assert "expects two arguments" in err


def test_sema_rejects_endswith_wrong_arg_count():
    err = sema_rejected('func main() { endswith("a"); }')
    assert "expects two arguments" in err


def test_sema_rejects_char_at_wrong_arg_count():
    err = sema_rejected('func main() { char_at("a"); }')
    assert "expects two arguments" in err


def test_sema_rejects_contains_wrong_arg_count():
    err = sema_rejected('func main() { contains("a"); }')
    assert "expects two arguments" in err


def test_sema_accepts_valid_builtin_usage():
    sema_accepted(
        'func main() {\n'
        '    let s = "hello";\n'
        '    print len(s);\n'
        '    print trim(s);\n'
        '    print upper(s);\n'
        '    print lower(s);\n'
        '    print contains(s, "ell");\n'
        '    print startswith(s, "he");\n'
        '    print endswith(s, "lo");\n'
        '    print replace(s, "hello", "world");\n'
        '    print char_at(s, 0);\n'
        '    let parts = split(s, "e");\n'
        '    print join(parts, ",");\n'
        '    print rand(10);\n'
        '    assert(1);\n'
        '    let n = abs(-5);\n'
        '    print min(n, 3);\n'
        '    print max(n, 3);\n'
        '    print to_str(n);\n'
        '    print substr(s, 1, 3);\n'
        '    print read("test.txt");\n'
        '    write("out.txt", s);\n'
        '    let x = input();\n'
        '    print argc();\n'
        '    print arg(0);\n'
        '}')
