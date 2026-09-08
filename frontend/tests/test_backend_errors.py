"""
Backend error reporting integration tests: codegen failures must carry the
source position (line/column) of the offending expression or statement.
"""
import re

from test_arrays import compile_only


def _assert_pos(error_text, message, line, col):
    m = re.search(r"{}\s*at line (\d+), column (\d+)".format(re.escape(message)), error_text)
    assert m, f"no position reported in: {error_text!r}"
    assert int(m.group(1)) == line, f"wrong line in {error_text!r}"
    assert int(m.group(2)) == col, f"wrong column in {error_text!r}"


def test_read_type_error_position():
    ec, err = compile_only('let a = 1;\nprint read(5);\n')
    assert ec != 0
    _assert_pos(err, "read() requires a string path", 2, 12)


def test_write_payload_type_error_position():
    ec, err = compile_only('let x = 0;\nwrite("f.txt", x);\n')
    assert ec != 0
    _assert_pos(err, "write() requires a string payload", 2, 16)


def test_exit_type_error_position():
    ec, err = compile_only('hot {\n    exit("bye");\n}\n')
    assert ec != 0
    _assert_pos(err, "exit() requires an integer code", 2, 10)


def test_unknown_function_position():
    ec, err = compile_only('hot {\n    let x = 1;\n    missing_fn(x);\n}\n')
    assert ec != 0
    _assert_pos(err, "Unknown function: missing_fn", 3, 5)


def test_nested_call_reports_arg_position():
    ec, err = compile_only('hot {\n    print arg(0, 1);\n}\n')
    assert ec != 0
    assert "arg" in err
    assert "at line 2, column " in err


def test_let_var_type_error_position():
    ec, err = compile_only('let s: int[] = [1, 2, 3];\nprint len(s);\n')
    assert ec == 0, err


def test_error_without_position_still_reports_message():
    # Sanity: even unpositioned errors expose the message text.
    ec, err = compile_only('hot { print wibble; }\n')
    assert ec != 0
    assert "Variable not found: wibble" in err