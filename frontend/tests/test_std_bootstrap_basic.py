from test_arrays import run_and_capture

def test_std_math_bootstrap():
    ec, out, err = run_and_capture('import "std:math";\nprint sqrt(25.0);\nprint gcd(15, 25);\n')
    assert ec == 0, err
    assert out == ["5.000000", "5"]

def test_std_text_bootstrap():
    ec, out, err = run_and_capture('import "std:text";\nprint repeat("ab", 3);\n')
    assert ec == 0, err
    assert out == ["ababab"]
