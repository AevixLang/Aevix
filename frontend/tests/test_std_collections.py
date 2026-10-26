"""
std:collections contract tests: monomorphic hash maps/sets and str_hash.

The maps and sets are arena-backed open-addressing hash tables; these tests
cover growth/rehash, overwrite, removal (which rebuilds the probe table), and
string keys. They also pin str_hash to a stable, non-negative value.
"""
from test_arrays import run_and_capture


def test_int_map_grows_and_reads():
    ec, out, err = run_and_capture("""
import "std:collections";
func main() {
    let m = new_map_int_int();
    let i = 0;
    while (i < 100) { map_int_int_put(m, i, i * 2); i = i + 1; }
    print(map_int_int_size(m));
    print(map_int_int_get(m, 0));
    print(map_int_int_get(m, 57));
    print(map_int_int_get(m, 99));
    print(map_int_int_get(m, 100));
}
main();
""")
    assert ec == 0, err
    assert out == ["100", "0", "114", "198", "0"]


def test_int_map_overwrite_and_remove():
    ec, out, err = run_and_capture("""
import "std:collections";
func main() {
    let m = new_map_int_int();
    map_int_int_put(m, 7, 1);
    map_int_int_put(m, 7, 2);
    print(map_int_int_get(m, 7));
    print(map_int_int_has(m, 7));
    map_int_int_remove(m, 7);
    print(map_int_int_size(m));
    print(map_int_int_has(m, 7));
}
main();
""")
    assert ec == 0, err
    assert out == ["2", "true", "0", "false"]


def test_int_map_keys_values_keep_insertion_order():
    ec, out, err = run_and_capture("""
import "std:collections";
func main() {
    let m = new_map_int_int();
    map_int_int_put(m, 3, 30);
    map_int_int_put(m, 1, 10);
    map_int_int_put(m, 2, 20);
    let ks = map_int_int_keys(m);
    let vs = map_int_int_values(m);
    print(ks[0]); print(ks[1]); print(ks[2]);
    print(vs[0]); print(vs[1]); print(vs[2]);
}
main();
""")
    assert ec == 0, err
    assert out == ["3", "1", "2", "30", "10", "20"]


def test_string_map_grows_and_removes():
    ec, out, err = run_and_capture("""
import "std:collections";
func main() {
    let m = new_map_str_int();
    let words = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"];
    let i = 0;
    while (i < 10) { map_str_int_put(m, words[i], i); i = i + 1; }
    print(map_str_int_size(m));
    print(map_str_int_get(m, "a"));
    print(map_str_int_get(m, "j"));
    print(map_str_int_has(m, "zz"));
    map_str_int_remove(m, "c");
    print(map_str_int_has(m, "c"));
    print(map_str_int_has(m, "d"));
}
main();
""")
    assert ec == 0, err
    assert out == ["10", "0", "9", "false", "false", "true"]


def test_int_set_dedup_and_remove():
    ec, out, err = run_and_capture("""
import "std:collections";
func main() {
    let s = new_set_int();
    let i = 0;
    while (i < 100) { set_int_add(s, i % 50); i = i + 1; }
    print(set_int_size(s));
    print(set_int_contains(s, 0));
    print(set_int_contains(s, 49));
    print(set_int_contains(s, 50));
    set_int_remove(s, 25);
    print(set_int_contains(s, 25));
    print(set_int_size(s));
}
main();
""")
    assert ec == 0, err
    assert out == ["50", "true", "true", "false", "false", "49"]


def test_string_set_dedup_and_remove():
    ec, out, err = run_and_capture("""
import "std:collections";
func main() {
    let s = new_set_str();
    set_str_add(s, "x");
    set_str_add(s, "y");
    set_str_add(s, "x");
    print(set_str_size(s));
    print(set_str_contains(s, "y"));
    set_str_remove(s, "x");
    print(set_str_contains(s, "x"));
    print(set_str_size(s));
}
main();
""")
    assert ec == 0, err
    assert out == ["2", "true", "false", "1"]


def test_str_hash_is_stable_and_non_negative():
    ec, out, err = run_and_capture("""
func main() {
    print(str_hash("abc") == str_hash("abc"));
    print(str_hash("abc") != str_hash("abd"));
    print(str_hash("") >= 0);
    print(str_hash("a long key with spaces") >= 0);
}
main();
""")
    assert ec == 0, err
    assert out == ["true", "true", "true", "true"]
