"""
Backend I/O feature integration tests: CLI arguments, exit code, file I/O.

Uses the same full-pipeline harness as test_arrays (frontend -> backend ->
llc -> clang -> run), additionally forwarding program arguments and seeding
input files into the run directory.
"""
from test_arrays import run_and_capture


# --- CLI arguments ---

def test_argc_arg_values():
    ec, out, err = run_and_capture(
        "hot { print argc(); print arg(0); print arg(1); }\n",
        args=("alpha", "beta"),
    )
    assert ec == 0, err
    assert out == ["2", "alpha", "beta"]


def test_no_args():
    ec, out, err = run_and_capture("hot { print argc(); }\n")
    assert ec == 0, err
    assert out == ["0"]


def test_arg_out_of_bounds_empty():
    ec, out, err = run_and_capture(
        "hot { print len(arg(0)); print arg(5); }\n", args=("x",)
    )
    assert ec == 0, err
    assert out == ["1", ""]


def test_arg_negative_empty():
    ec, out, err = run_and_capture(
        "hot { print len(arg(-1)); }\n", args=("a", "b")
    )
    assert ec == 0, err
    assert out == ["0"]


def test_arg_compare_and_concat():
    ec, out, err = run_and_capture(
        'hot { if (arg(0) == "hi") { print "eq"; } print arg(0) + arg(1); }\n',
        args=("hi", "!"),
    )
    assert ec == 0, err
    assert out == ["eq", "hi!"]


def test_io_reachable_from_function():
    ec, out, err = run_and_capture(
        "func combine() : string { return arg(0) + arg(1); }\n"
        "hot { print combine(); }\n",
        args=("a", "b"),
    )
    assert ec == 0, err
    assert out == ["ab"]


# --- exit code ---

def test_exit_status_code():
    ec, out, err = run_and_capture(
        'hot { print "before"; exit(7); print "after"; }\n'
    )
    assert ec == 7
    assert out == ["before"]


def test_exit_zero_is_success():
    ec, out, err = run_and_capture("hot { exit(0); }\n")
    assert ec == 0


def test_exit_conditional():
    ec, out, err = run_and_capture(
        'hot { if (arg(0) == "fail") { exit(3); } else { exit(0); } }\n',
        args=("fail",),
    )
    assert ec == 3


def test_exit_accepts_expression():
    ec, out, err = run_and_capture("hot { let n = 2; exit(1 + n * 3); }\n")
    assert ec == 7


# --- file I/O ---

def test_read_file():
    ec, out, err = run_and_capture(
        'hot { let s = read("in.txt"); print len(s); print s; }\n',
        files={"in.txt": "hello, world!"},
    )
    assert ec == 0, err
    assert out == ["13", "hello, world!"]


def test_read_missing_file_empty():
    ec, out, err = run_and_capture(
        'hot { print len(read("nope.txt")); }\n'
    )
    assert ec == 0, err
    assert out == ["0"]


def test_read_empty_file():
    ec, out, err = run_and_capture(
        'hot { print len(read("empty.txt")); }\n', files={"empty.txt": ""}
    )
    assert ec == 0, err
    assert out == ["0"]


def test_write_read_roundtrip():
    ec, out, err = run_and_capture(
        'hot { write("out.txt", "data:" + arg(0)); print read("out.txt"); }\n',
        args=("x",),
    )
    assert ec == 0, err
    assert out == ["data:x"]


def test_write_returns_bool():
    ec, out, err = run_and_capture(
        'hot { let ok = write("f.txt", "z"); print ok; if (ok) { print "yes"; } }\n'
    )
    assert ec == 0, err
    assert out == ["1", "yes"]


def test_read_into_string_operations():
    ec, out, err = run_and_capture(
        'hot { let s = read("greet.txt"); print "[" + s + "]"; }\n',
        files={"greet.txt": "hi"},
    )
    assert ec == 0, err
    assert out == ["[hi]"]


# --- stdin input() ---

def test_input_reads_line():
    ec, out, err = run_and_capture(
        'hot { print input(); }\n', stdin="hello"
    )
    assert ec == 0, err
    assert out == ["hello"]


def test_input_strips_leading_newline_keeps_payload():
    ec, out, err = run_and_capture(
        'hot { print input(); print len(input()); }\n', stdin="abc\ndef\n"
    )
    assert ec == 0, err
    assert out == ["abc", "3"]


def test_input_second_call_reads_next_line():
    ec, out, err = run_and_capture(
        'hot { print input() + ":" + input(); }\n', stdin="a\nb"
    )
    assert ec == 0, err
    assert out == ["a:b"]


def test_input_eof_empty():
    ec, out, err = run_and_capture(
        'hot { print len(input()); print input(); }\n'
    )
    assert ec == 0, err
    assert out == ["0", ""]


def test_input_empty_line_is_empty_string():
    ec, out, err = run_and_capture(
        'hot { print len(input()); }\n', stdin="\n"
    )
    assert ec == 0, err
    assert out == ["0"]


def test_input_in_string_operations():
    ec, out, err = run_and_capture(
        'hot { print "[" + input() + "]"; }\n', stdin="name"
    )
    assert ec == 0, err
    assert out == ["[name]"]


def test_input_write_read_roundtrip():
    ec, out, err = run_and_capture(
        'hot { let s = input(); write("out.txt", s); print read("out.txt"); }\n',
        stdin="payload",
    )
    assert ec == 0, err
    assert out == ["payload"]