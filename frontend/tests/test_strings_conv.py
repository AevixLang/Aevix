"""
String conversion & escape integration tests: \n \t \\ \" escapes, to_int/
to_float/to_str, substr, and the min/max/abs math helpers. Full pipeline.
"""
from test_arrays import run_and_capture


# --- escape sequences ---

def test_escape_newline():
    ec, out, err = run_and_capture('print "a\\nb";\n')
    assert ec == 0, err
    assert out == ["a", "b"]


def test_escape_tab():
    ec, out, err = run_and_capture('print "a\\tb";\n')
    assert ec == 0, err
    assert out == ["a\tb"]


def test_escape_backslash():
    ec, out, err = run_and_capture('print "a\\\\b";\n')
    assert ec == 0, err
    assert out == ["a\\b"]


def test_escape_quote():
    ec, out, err = run_and_capture('print "say \\"hi\\"";\n')
    assert ec == 0, err
    assert out == ['say "hi"']


def test_escape_counts_in_length():
    ec, out, err = run_and_capture('print len("ab\\ncd");\n')
    assert ec == 0, err
    assert out == ["5"]


def test_escape_carriage_return_by_length():
    ec, out, err = run_and_capture('print len("a\\rb");\n')
    assert ec == 0, err
    assert out == ["3"]


def test_unknown_escape_kept_verbatim():
    ec, out, err = run_and_capture('print len("a\\zb");\n')
    assert ec == 0, err
    assert out == ["4"]


def test_multiple_escapes_and_concat():
    ec, out, err = run_and_capture('print "x\\n" + "y\\t" + "z";\n')
    assert ec == 0, err
    assert out == ["x", "y\tz"]


# --- to_int ---

def test_to_int_basic():
    ec, out, err = run_and_capture('print to_int("42") + 1;\n')
    assert ec == 0, err
    assert out == ["43"]


def test_to_int_negative():
    ec, out, err = run_and_capture('print to_int("-17");\n')
    assert ec == 0, err
    assert out == ["-17"]


def test_to_int_stops_at_non_digit():
    ec, out, err = run_and_capture('print to_int("  7abc");\n')
    assert ec == 0, err
    assert out == ["7"]


def test_to_int_empty_zero():
    ec, out, err = run_and_capture('print to_int("");\n')
    assert ec == 0, err
    assert out == ["0"]


def test_to_int_decimal_truncates():
    ec, out, err = run_and_capture('print to_int("3.14");\n')
    assert ec == 0, err
    assert out == ["3"]


# --- to_float ---

def test_to_float_basic():
    ec, out, err = run_and_capture('print to_float("3.5") + 1.0;\n')
    assert ec == 0, err
    assert out == ["4.500000"]


def test_to_float_negative():
    ec, out, err = run_and_capture('print to_float("-2.25");\n')
    assert ec == 0, err
    assert out == ["-2.250000"]


def test_to_float_exponent():
    ec, out, err = run_and_capture('print to_float("1e3");\n')
    assert ec == 0, err
    assert out == ["1000.000000"]


# --- to_str ---

def test_to_str_int():
    ec, out, err = run_and_capture('let s = to_str(42); print s; print len(s);\n')
    assert ec == 0, err
    assert out == ["42", "2"]


def test_to_str_concat():
    ec, out, err = run_and_capture('print "n=" + to_str(5) + "!";\n')
    assert ec == 0, err
    assert out == ["n=5!"]


def test_to_str_float():
    ec, out, err = run_and_capture('print to_str(3.5);\n')
    assert ec == 0, err
    assert out == ["3.500000"]


def test_to_str_negative_int():
    ec, out, err = run_and_capture('print to_str(-12);\n')
    assert ec == 0, err
    assert out == ["-12"]


def test_to_str_built_line_with_escape():
    ec, out, err = run_and_capture(
        'let line = "v=" + to_str(9) + "\\n"; print line;\n'
    )
    assert ec == 0, err
    assert out == ["v=9", ""]


# --- substr ---

def test_substr_middle():
    ec, out, err = run_and_capture('print substr("hello", 1, 3);\n')
    assert ec == 0, err
    assert out == ["ell"]


def test_substr_to_end():
    ec, out, err = run_and_capture('print substr("hello", 1, 100);\n')
    assert ec == 0, err
    assert out == ["ello"]


def test_substr_from_start():
    ec, out, err = run_and_capture('print substr("hello", 0, 2);\n')
    assert ec == 0, err
    assert out == ["he"]


def test_substr_past_end_empty():
    ec, out, err = run_and_capture('print len(substr("hello", 10, 3));\n')
    assert ec == 0, err
    assert out == ["0"]


def test_substr_negative_start_clamped():
    ec, out, err = run_and_capture('print substr("abc", -1, 2);\n')
    assert ec == 0, err
    assert out == ["ab"]


def test_substr_concat_roundtrip():
    ec, out, err = run_and_capture(
        'let s = substr("hello", 1, 2) + substr("hello", 0, 1); print s;\n'
    )
    assert ec == 0, err
    assert out == ["elh"]


# --- min / max / abs ---

def test_min_max_int():
    ec, out, err = run_and_capture('print min(3, 7); print max(3, 7);\n')
    assert ec == 0, err
    assert out == ["3", "7"]


def test_min_max_float():
    ec, out, err = run_and_capture('print min(2.5, 1.5); print max(2.5, 1.5);\n')
    assert ec == 0, err
    assert out == ["1.500000", "2.500000"]


def test_min_max_mixed_promotes():
    ec, out, err = run_and_capture('print min(3, 4.0); print max(3, 4.0);\n')
    assert ec == 0, err
    assert out == ["3.000000", "4.000000"]


def test_abs_int():
    ec, out, err = run_and_capture('print abs(-5); print abs(5);\n')
    assert ec == 0, err
    assert out == ["5", "5"]


def test_abs_float():
    ec, out, err = run_and_capture('print abs(-2.5);\n')
    assert ec == 0, err
    assert out == ["2.500000"]


def test_min_max_abs_in_expressions():
    ec, out, err = run_and_capture(
        'let n = min(10, max(2, 8)); print n; print n + abs(-4);\n'
    )
    assert ec == 0, err
    assert out == ["8", "12"]