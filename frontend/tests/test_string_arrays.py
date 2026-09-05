"""
Stage 2 integration tests: string[] arrays and the split() builtin.
Covers literals, new, indexing/assignment, len, print, params/returns,
fixed string[N] arrays, for-in, and split edge cases. Full pipeline.
"""
from test_arrays import run_and_capture


# --- split() basics ---

def test_split_basic():
    ec, out, err = run_and_capture(
        'let parts = split("alpha,beta,gamma", ",");\n'
        "print parts;\n"
        "print len(parts);\n"
    )
    assert ec == 0, err
    assert out == ['["alpha", "beta", "gamma"]', "3"]


def test_split_index():
    ec, out, err = run_and_capture(
        'let parts = split("a,b,c", ",");\n'
        "print parts[0];\n"
        "print parts[2];\n"
    )
    assert ec == 0, err
    assert out == ["a", "c"]


def test_split_empty_parts_preserved():
    ec, out, err = run_and_capture(
        'let parts = split("a,,b", ",");\n'
        "print parts;\n"
        "print len(parts);\n"
    )
    assert ec == 0, err
    assert out == ['["a", "", "b"]', "3"]


def test_split_trailing_separator():
    ec, out, err = run_and_capture(
        'let parts = split("a,b,", ",");\n'
        "print parts;\n"
    )
    assert ec == 0, err
    assert out == ['["a", "b", ""]']


def test_split_no_occurrence():
    ec, out, err = run_and_capture(
        'let parts = split("no-separator", ",");\n'
        "print parts;\n"
        "print len(parts);\n"
    )
    assert ec == 0, err
    assert out == ['["no-separator"]', "1"]


def test_split_empty_separator():
    ec, out, err = run_and_capture(
        'let parts = split("single", "");\n'
        "print parts;\n"
        "print len(parts);\n"
    )
    assert ec == 0, err
    assert out == ['["single"]', "1"]


def test_split_multibyte_separator():
    ec, out, err = run_and_capture(
        'let parts = split("x::y::z", "::");\n'
        "print parts;\n"
    )
    assert ec == 0, err
    assert out == ['["x", "y", "z"]']


# --- string[] as a value ---

def test_string_array_assignment():
    ec, out, err = run_and_capture(
        'let parts = split("1 2 3", " ");\n'
        'parts[1] = "TWO";\n'
        "print parts;\n"
    )
    assert ec == 0, err
    assert out == ['["1", "TWO", "3"]']


def test_new_string_array():
    ec, out, err = run_and_capture(
        'let a = new string[2];\n'
        'a[0] = "first";\n'
        'a[1] = "second";\n'
        "print a;\n"
    )
    assert ec == 0, err
    assert out == ['["first", "second"]']


def test_len_string_array():
    ec, out, err = run_and_capture(
        'let a = new string[3];\n'
        "print len(a);\n"
    )
    assert ec == 0, err
    assert out == ["3"]


def test_string_array_param_and_return():
    ec, out, err = run_and_capture(
        "func count(parts: string[]) : int {\n"
        "    return len(parts);\n"
        "}\n"
        'print count(split("a,b,c,d", ","));\n'
    )
    assert ec == 0, err
    assert out == ["4"]


def test_string_array_local_literal():
    ec, out, err = run_and_capture(
        'let a: string[3] = ["red", "green", "blue"];\n'
        "print a;\n"
        "print a[2];\n"
    )
    assert ec == 0, err
    assert out == ['["red", "green", "blue"]', "blue"]


def test_string_array_concat_loop():
    ec, out, err = run_and_capture(
        'let parts = split("x:y:z", ":");\n'
        'let joined: string = "";\n'
        "let i: int = 0;\n"
        "while (i < len(parts)) {\n"
        '    if (i > 0) { joined = joined + "-"; }\n'
        "    joined = joined + parts[i];\n"
        "    i = i + 1;\n"
        "}\n"
        "print joined;\n"
    )
    assert ec == 0, err
    assert out == ["x-y-z"]


# --- for-in over string[] ---

def test_for_in_split():
    ec, out, err = run_and_capture(
        'for w in split("one two three", " ") {\n'
        "    print w;\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["one", "two", "three"]


def test_for_in_string_array_var():
    ec, out, err = run_and_capture(
        'let words: string[2] = ["hi", "there"];\n'
        "for w in words {\n"
        "    print w;\n"
        "}\n"
    )
    assert ec == 0, err
    assert out == ["hi", "there"]


# --- error cases ---

def test_split_requires_string():
    code, _, err = run_and_capture("let p = split(42, \",\");\n")
    assert code != 0
    assert "string" in err


def test_split_separator_requires_string():
    code, _, err = run_and_capture('let p = split("a,b", 5);\n')
    assert code != 0
    assert "string" in err