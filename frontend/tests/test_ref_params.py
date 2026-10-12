"""
Ref-parameter contract tests.

A `ref` parameter (the single mutation-by-reference mechanism of Aevix) must:

  - bind to the caller's variable, so writes through the parameter are
    visible after the call (by-value params keep a copy);
  - accept any lvalue: variable, struct field and array element;
  - be forwardable from one ref parameter to another without losing the
    binding;
  - reject at sema: rvalue arguments, two refs bound to the same variable,
    and `ref` on an open array parameter.

Positive cases run the full pipeline and check runtime output; negative
cases drive frontend + aevix-sema and assert the diagnostic.
"""
import pytest

from test_arrays import run_and_capture
from test_sema import sema_rejected, sema_accepted


# --- runtime: by-value vs ref -------------------------------------------------

def test_ref_mutates_caller_variable():
    ec, out, err = run_and_capture(
        "func scale_copy(v: int): int { v = v * 2; return v; }\n"
        "func scale(v: ref int) { v = v * 2; }\n"
        "hot {\n"
        "    let v = 4;\n"
        "    let copy = scale_copy(v);\n"
        "    print \"copy\", v, copy;\n"
        "    scale(v);\n"
        "    print \"after\", v;\n"
        "}")
    assert ec == 0, err
    assert out == ["copy 4 8", "after 8"]


def test_ref_swap():
    ec, out, err = run_and_capture(
        "func swap(a: ref int, b: ref int) {\n"
        "    let t = a; a = b; b = t;\n"
        "}\n"
        "hot {\n"
        "    let a = 1; let b = 2;\n"
        "    swap(a, b);\n"
        "    print a, b;\n"
        "}")
    assert ec == 0, err
    assert out == ["2 1"]


def test_ref_struct_field():
    ec, out, err = run_and_capture(
        "struct Ctr { value: int }\n"
        "func tick(c: ref Ctr) { c.value = c.value + 1; }\n"
        "hot {\n"
        "    let ctr = Ctr { 0 };\n"
        "    tick(ctr);\n"
        "    tick(ctr);\n"
        "    print ctr.value;\n"
        "}")
    assert ec == 0, err
    assert out == ["2"]


def test_ref_array_element():
    ec, out, err = run_and_capture(
        "func bump(x: ref int) { x = x + 10; }\n"
        "hot {\n"
        "    let arr = new int[3];\n"
        "    arr[1] = 2;\n"
        "    bump(arr[1]);\n"
        "    print arr[1];\n"
        "}")
    assert ec == 0, err
    assert out == ["12"]


def test_ref_param_forwarded_to_ref_param():
    ec, out, err = run_and_capture(
        "func bump(x: ref int) { x = x + 10; }\n"
        "func twice(x: ref int) { bump(x); bump(x); }\n"
        "hot {\n"
        "    let v = 5;\n"
        "    twice(v);\n"
        "    print v;\n"
        "}")
    assert ec == 0, err
    assert out == ["25"]


# --- sema rejection: rvalue arguments -----------------------------------------

def test_sema_rejects_literal_to_ref():
    err = sema_rejected("func incr(x: ref int) { x = x + 1; }\n"
                        "func main() { incr(1); }")
    assert "ref parameter requires an lvalue argument" in err


def test_sema_rejects_expression_to_ref():
    err = sema_rejected("func incr(x: ref int) { x = x + 1; }\n"
                        "func main() { let a = 1; incr(a + a); }")
    assert "ref parameter requires an lvalue argument" in err


def test_sema_rejects_any_rvalue_element_to_ref():
    err = sema_rejected("func swap(a: ref int, b: ref int) {}\n"
                        "func main() { let x = 1; swap(x, 0); }")
    assert "ref parameter requires an lvalue argument" in err


# --- sema rejection: aliasing and shape ---------------------------------------

def test_sema_rejects_two_refs_to_same_variable():
    err = sema_rejected("func pair(a: ref int, b: ref int) {}\n"
                        "func main() { let x = 1; pair(x, x); }")
    assert "two ref parameters bound to the same variable" in err


def test_sema_rejects_refs_to_same_root_via_field():
    err = sema_rejected("struct C { v: int }\n"
                        "func pair(a: ref int, b: ref int) {}\n"
                        "func main() { let c = C { 1 }; pair(c.v, c.v); }")
    assert "two ref parameters bound to the same variable" in err


def test_sema_rejects_ref_open_array_parameter():
    err = sema_rejected("func tail(xs: ref int[]) {}\n"
                        "func main() {}")
    assert "open array parameters cannot be passed by reference" in err


# --- sema acceptance: ref is legal on values and on other refs -----------------

def test_sema_accepts_ref_parameter_forms():
    sema_accepted(
        "struct C { v: int }\n"
        "func m1(a: ref int) { a = a + 1; }\n"
        "func m2(c: ref C) { c.v = c.v + 1; }\n"
        "func m3(a: ref int, c: ref C) { m1(a); m2(c); }\n"
        "func main() {\n"
        "    let a = 1;\n"
        "    let c = C { 1 };\n"
        "    m3(a, c);\n"
        "    print a, c.v;\n"
        "}")