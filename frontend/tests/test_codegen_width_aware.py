"""
Width-aware scalar codegen integration tests: fixed-width ints, unsigned
arithmetic/compare, compound-assign wraps, enums as i32 indices, and f32
printing, together with the widening-only implicit-coercion rules.

Covers i64 literal materialisation, u32/u64 literals beyond i32 range,
unsigned comparisons and arithmetic, signed i8 narrowing at link time,
f32 arithmetic, enum variant print/comparison, unsigned compound
assignment semantics (wrap in the target width, never signedness change),
and the implicit widening-only coercion rule (narrowing rejected).
"""
from test_arrays import compile_only, run_and_capture


# --- i64 / u64 literals ---

def test_i64_large_literal_runtime():
    ec, out, err = run_and_capture("let x: i64 = 3000000000;\nprint x;\n")
    assert ec == 0, err
    assert out == ["3000000000"]


def test_i64_wide_expression_runtime():
    ec, out, err = run_and_capture(
        "let a: i64 = 2000000000;\n"
        "let b: i64 = 2000000000;\n"
        "print a + b;\n"
        "let c = a * 2;\n"
        "print c;\n"
    )
    assert ec == 0, err
    assert out == ["4000000000", "4000000000"]


def test_u64_max_runtime():
    ec, out, err = run_and_capture(
        "let x: u64 = 18446744073709551615;\nprint x;\n"
    )
    assert ec == 0, err
    assert out == ["18446744073709551615"]


def test_i64_to_string_negative_range():
    ec, out, err = run_and_capture("let x: i64 = -9000000000;\nprint x;\n")
    assert ec == 0, err
    assert out == ["-9000000000"]


def test_i64_widening_widen_literal():
    # 2000000000 fits i32, but "int" (i32) alone cannot hold 4000000000;
    # the common type of two i64 operands keeps the arithmetic in i64.
    ec, out, err = run_and_capture(
        "let a: i64 = 2000000000;\n"
        "let b: i64 = 2000000000;\n"
        "print a * b;\n"
    )
    assert ec == 0, err
    assert out == ["4000000000000000000"]


# --- u32 / unsigned comparisons ---

def test_u32_large_literal_arith():
    ec, out, err = run_and_capture(
        "let a: u32 = 3950000000;\n"
        "let b: u32 = 4000000000;\n"
        "print a < b;\n"
        "print b - a;\n"
    )
    assert ec == 0, err
    assert out == ["true", "50000000"]


def test_unsigned_compare_variants():
    ec, out, err = run_and_capture(
        "let a: u32 = 3;\n"
        "let b: u32 = 4294967295;\n"
        "print a > b;\n"
        "print a < b;\n"
        "let c: u16 = 65535;\n"
        "print c > 0;\n"
    )
    assert ec == 0, err
    assert out == ["false", "true", "true"]


def test_u8_unsigned_compare_lit():
    # 8-bit literals emit unsigned compares (ULT/UGT); this is what makes
    # `255 > 0` come out true instead of being misread as negative -1.
    ec, out, err = run_and_capture(
        "let a: u8 = 255;\nprint a > 0;\n"
    )
    assert ec == 0, err
    assert out == ["true"]


# --- signed i8 arithmetic (sign-extends at link time) ---

def test_i8_arith_narrowing_vars():
    # i8+i8 stays in i8 width (wraps); it widens to int only on assignment
    # to an int-typed target. Both the narrow compare and the widened sum
    # participate without an explicit to_* call.
    ec, out, err = run_and_capture(
        "let a: i8 = 100;\n"
        "let b: i8 = -100;\n"
        "print a > b;\n"
        "let r: int = a + b;\n"
        "print r;\n"
    )
    assert ec == 0, err
    assert out == ["true", "0"]


# --- f32 ---

def test_f32_arith():
    ec, out, err = run_and_capture(
        "let a: f32 = 1.5;\n"
        "let b: f32 = 2.25;\n"
        "print a + b;\n"
        "print a * b;\n"
    )
    assert ec == 0, err
    assert out == ["3.750000", "3.375000"]


# --- enums: variant inference, equality, printing ---

def test_enum_print_and_compare():
    ec, out, err = run_and_capture(
        "enum Color { red, green, blue }\n"
        "let c = Color.green;\n"
        "print c;\n"
        "if (c == Color.green) { print \"yes\"; }\n"
        "if (c == Color.red) { print \"no\"; }\n"
        "let n: Color = Color.blue;\n"
        "print n;\n"
    )
    assert ec == 0, err
    assert out == ["green", "yes", "blue"]


# --- unsigned compound assignment: wrap, never change signedness ---

def test_u64_compound_add_no_wrap():
    ec, out, err = run_and_capture(
        "let s: u64 = 1000000000;\n"
        "let z: u64 = 2000000000;\n"
        "s += z;\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["3000000000"]


def test_u32_compound_wraps_in_target_width():
    # *= takes an unsigned RHS operand of the same type; the mul wraps in
    # the 32-bit target width (4000000000 * 10 mod 2^32 = 1345294336).
    ec, out, err = run_and_capture(
        "let sq: u32 = 4000000000;\n"
        "let t: u32 = 10;\n"
        "sq *= t;\n"
        "print sq;\n"
    )
    assert ec == 0, err
    assert out == ["1345294336"]


def test_compound_preserves_unsigned_augend():
    ec, out, err = run_and_capture(
        "let s: u64 = 1000000000;\n"
        "s += 4000000000;\n"
        "print s;\n"
    )
    assert ec == 0, err
    assert out == ["5000000000"]


# --- coercion rules: implicit widening only ---

def test_implicit_narrowing_float_to_int_rejected():
    ec, err = compile_only("let a: int[3] = [1.5, 2.5, 3.5];\n")
    assert ec != 0, "implicit float->int narrowing must be rejected"
    assert "mismatch" in err.lower()


def test_implicit_int_const_wraps_into_i8_with_no_width_error():
    # A type-annotated i8 = <int literal> is NOT a width error: the value
    # wraps into the target width (300 mod 256 = 44 -> ',').
    ec, out, err = run_and_capture("let b: i8 = 300;\nprint b;\n")
    assert ec == 0, err
    assert out == [","]


def test_implicit_int_var_narrowing_rejected():
    # Narrowing an i32 VARIABLE to i8 is not implicit — it requires to_ .
    ec, err = compile_only("let x: int = 300;\nlet b: i8 = x;\n")
    assert ec != 0, "i32 variable narrowing to i8 must be rejected"
    assert "convert" in err.lower()


def test_mixed_inferred_array_rejected():
    ec, err = compile_only("let a = [1, 2.5];\n")
    assert ec != 0, "mixed-type array literal must be rejected"


def test_widening_int_to_i64_allowed():
    # Int -> i64 is a permitted implicit widening.
    ec, out, err = run_and_capture(
        "let a: i64 = 1;\n"
        "let small = 2;\n"
        "print a + small;\n"
    )
    assert ec == 0, err
    assert out == ["3"]


def test_arg_type_mismatch_rejected():
    ec, err = compile_only('func take_float(f: float) {}\n'
                           'func main() { take_float("oops"); }\n')
    assert ec != 0, "passing a string to a float parameter must be rejected"
    assert "Argument" in err
