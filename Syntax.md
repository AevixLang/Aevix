# Aevix Language Syntax

This document provides the complete specification for the Aevix language (v0.1).

## ⚡ Quick Start
```aev
func calculate_sum(limit: int) : int {
    let total = 0;
    for (let i = 0; i < limit; i += 1) {
        total += i;
    }
    return total;
}

let result = calculate_sum(100);
print result;
```

---

## 1. Type System
Aevix is a statically typed language. Types can be declared explicitly or inferred.

| Type | Description | LLVM Mapping |
| :--- | :--- | :--- |
| `int` | 32-bit signed integer | `i32` |
| `float` | Double-precision float (alias for `f64`) | `double` |
| `bool` | Boolean (`true` / `false`) | `i1` |
| `string` | UTF-8 string slice (`{ i8*, i32 }`) | `{ i8*, i32 }` |
| `i8`, `i16`, `i32`, `i64` | Signed fixed-width integers | `i8`, `i16`, `i32`, `i64` |
| `u8`, `u16`, `u32`, `u64` | Unsigned fixed-width integers | `i8`, `i16`, `i32`, `i64` |
| `f32` | 32-bit float | `float` |
| `f64` | 64-bit float | `double` |

Fixed-width types are auto-promoted to `int`/`float` in arithmetic. Assigning a wider type to a narrower target is a compile error unless the value fits.

---

## 2. Variables and Assignments

### Declarations
Use the `let` keyword. Type inference is supported.
```aev
let x: int = 10;      // Explicit
let y = 20.5;         // Inferred as float
let active = true;    // Inferred as bool
```

### Updates
Existing variables are updated via simple assignment:
```aev
x = 15;
```

Compound assignment is supported for the arithmetic operators on numeric
targets (variables, array elements, struct fields):
```aev
x += 5;      // x = x + 5
x -= 3;      // x = x - 3
x *= 4;      // x = x * 4
x /= 2;      // x = x / 2
```
An `int` value is auto-promoted to `float` when the target is `float`
(`f += 2`); assigning a `float` into an `int` target is an error. Compound
assignment on strings, booleans or open arrays (slices) is rejected.

---

## 3. Expressions

### Operators
| Category | Operators | Description |
| :--- | :--- | :--- |
| **Arithmetic** | `+`, `-`, `*`, `/` | Basic math |
| **Comparison** | `==`, `!=`, `<`, `>`, `<=`, `>=` | Boolean results |
| **Logic** | `&&`, `||`, `!` | Boolean algebra |
| **Unary** | `-`, `!` | Negation / Inversion |

### Operator Precedence
Ordered from highest to lowest priority:
1. `()` Parentheses
2. `!`, `-` Unary operators
3. `*`, `/` Multiplication and Division
4. `+`, `-` Addition and Subtraction
5. `==`, `!=`, `<`, `>`, `<=`, `>=` Comparisons
6. `&&` Logical AND
7. `||` Logical OR

---

## 4. Control Flow

### Conditional (`if`)
```aev
if (condition) {
    // then block
} else {
    // else block (optional)
}
```

### Pattern match (`match`)
```aev
match (expr) {
    case value: { /* body */ }
    case value: { /* body */ }
    default:    { /* body */ }
}
```

`match` compares the scrutinee by equality and runs the first matching
branch's body; `default` is optional. Each branch must wrap its body in
braces. Values may be literals, variables, or enum-qualified names
(`Color.red`).

`match` also has an expression form that yields a value:

```aev
let y = match (x) {
    case 1 => 10,
    case 2 => 20,
    default => 30
};
```

Each arm uses `=>` and yields a single expression; all branches must
have the same type. `default` is required for non-enum scrutinees and
may be omitted when all enum variants are covered.

```aev
while (condition) {
    // body
}
```

### Iteration (`for`)
C-style loop: `for (init; condition; step) { body }`
```aev
for (let i = 0; i < 10; i += 1) {
    print i;
}
```
Any clause may be omitted (`for (;;)`, `for (; i < 5; i += 1)`, ...) and the
step may use compound assignment.

### Iteration (`for-in`)
Iterate over array elements: `for var in array { body }`
```aev
let nums = [10, 20, 30];
for x in nums {
    print x;
}
```

### `break` / `continue`
`break` exits the innermost loop; `continue` jumps to the next iteration
(the step for a `for` loop, the condition for `while`/`for-in`). Both are only
valid inside a loop — anywhere else is a compile error.
```aev
for (let i = 0; i < 10; i += 1) {
    if (i == 3) { continue; }     // skip 3
    if (i == 6) { break; }        // stop at 6
    print i;
}
```

---

## 5. Functions
Functions are defined using the `func` keyword. The return type is optional (defaults to `void`).

```aev
func power(base: float, exp: float) : float {
    return base * exp;
}
```

### Reference parameters
Prefix a parameter with `ref` to pass it by reference (read-write). The argument must be an lvalue (variable or assignable expression); temporaries, enum variants, and two `ref` params bound to the same variable are compile errors. `ref` is written on the parameter declaration only — a call site passes the plain argument.

```aev
func swap(a: ref int, b: ref int) {
    let tmp = a;
    a = b;
    b = tmp;
}

let x = 1;
let y = 2;
swap(x, y);
print x;  // 2
print y;  // 1
```

---

## 6. Modules and imports

A source file can pull declarations (`func` / `struct`) from other files:
```aev
import "utils.aev";            // path relative to the importing file
import "geometry/circle";      // ".aev" is appended when missing
import "submit:io";            // package "submit", module "io" (via aevix.lock)
import "utils.aev" as u;       // namespace the module behind an alias
```

Rules:
- Imports are only allowed at the top level of a file.
- Path-spec resolution order: relative to the importing file → the project's
  `lib/` directory → the directories in `$AEVIX_PATH`.
- A spec with `:` is a package import (`package:module`); the compiler finds
  the package through the project's `aevix.lock` (written by the package
  manager). `std:` is reserved for the future standard library.
- Without `as`, imported **declarations** are merged into one flat program.
  Top-level statements of a module (its side effects) are dropped.
- With `as alias`, every exported declaration is scoped behind that alias:
  call it as `alias.fn(...)`, name its types as `alias.Point`, construct them
  as `alias.Point { ... }`, and reach enum variants as `alias.Color.red`.
  Two modules may therefore export the same name under different aliases.
  A module may be imported under more than one alias, and a module imported
  without `as` still merges flat.
- A file imported more than once is included only once (dedup by real path).
- Circular imports are an error.
- A name defined in two different files is a conflict — an error — unless both
  modules are imported under aliases, which keeps them separate.
- `as` and `import` are reserved words (they cannot be used as identifiers).

### Package manager (`aevix-pm`)

The compiler only *reads* the package graph; the `aevix-pm` tool *writes* it.
`aevix-pm` maintains two files at the project root:

- `aevix.toml` — the manifest: your package's name, version, and declared
  dependencies (human-edited intent).
- `aevix.lock` — the resolved, pinned graph (JSON): the exact version and path
  of every dependency. This is what the compiler consumes.

```bash
aevix-pm init myapp                    # scaffold aevix.toml + aevix.lock
aevix-pm add calc --path ../calc       # add a local path dependency
aevix-pm remove calc                   # drop it and re-resolve the lock
aevix-pm list                          # show declared dependencies
```

`aevix add` records the dependency in `aevix.toml` and regenerates `aevix.lock`
(deterministically, so diffs stay minimal). A package is a directory of `.aev`
files; `import "calc:"` (empty module) loads its entry point `lib.aev`, while
`import "calc:num"` loads `num.aev` inside it. Local `path` dependencies are
supported in this release; version-based registry fetching lands later.

---

## 7. Arrays and Slices

Fixed-size arrays copy by value; open arrays (`int[]`) are slices `{ data*, len }` allocated in the arena.

```aev
let a: int[3] = [1, 2, 3];       // fixed array, copied by value
print a[1];                      // 2

let b: int[] = [4, 5, 6];        // open array (slice) — copied into the arena
print len(b);                    // 3
b[0] = 9;                        // mutation is visible through the slice
```

`for-in` iterates any array or slice:
```aev
let nums: int[] = [10, 20, 30];
for x in nums { print x; }
```

### `new` — arena allocation
```aev
let n = 5;
let buf = new int[n];            // runtime-sized, zeroed
buf[0] = 42;
let pts = new Point[2];          // array of structs, zeroed
```

Out-of-range array access aborts the program with a bounds error at runtime.

---

## 8. Strings

Strings are slices of `i8`, so they are values: comparable, concatenable, indexable.

```aev
let s: string = "hello";
print s;                 // hello  (no quotes)
print len(s);            // 5
print s[0];              // h  (a char; usable as int: s[0] + 1)
let t = s + " world";    // concatenation (allocated in the arena)
if (s == "hello") { ... }
if (s != "hello") { ... }
```

Strings work as function parameters/returns and struct fields. Arrays of
strings are supported in all the same shapes as `int`: open `string[]` slices,
fixed `string[N]` arrays, `new string[n]`, struct fields and parameters:

```aev
let parts = split("a,b,c", ",");      // -> string[] slice (arena-allocated)
print parts;                          // ["a", "b", "c"]
print parts[1];                       // b
parts[2] = "z";                       // mutable elements
print len(parts);                     // 3
for w in parts { print w; }           // iteration
let words: string[2] = ["x", "y"];    // fixed array
let fresh = new string[2];            // zeroed: ["", ""]
```

`split(s, sep)` splits `s` on every occurrence of `sep` and returns a
`string[]` slice. Empty pieces are preserved (`"a,,b"` by `","` gives
`["a", "", "b"]`); an empty separator returns `[s]` unchanged.

String literals support the escapes `\n`, `\t`, `\r`, `\\` and `\"`; any other
backslash sequence is kept verbatim:

```aev
let msg = "line 1\nline 2\t-tabbed";
```

`to_int`/`to_float` parse the leading number of a string; `to_str` formats
int/float values back into strings, so numbers and text compose freely:

---

## 9. Structs

```aev
struct Point { x: int, y: int }
struct Bag { items: int[], tag: string }

let p = Point { 1, 2 };                    // positional literal
print p.x + p.y;                           // 3

let q = Point { y: 2, x: 1 };              // named fields, any order
print q;                                   // {1, 2}

let b = Bag { new int[3], "box" };         // open-array fields allowed
b.items[1] = 55;
print b;                                   // {[0, 55, 0], box}

let c = Bag { tag: "bag", items: new int[3] };
```

Struct fields are accessed with `.`; member access chains and arrays of structs work.

Struct literals may be fully positional or fully named — the two styles cannot be
mixed inside one literal. Named fields may appear in any order, but every field
must be given exactly once; there are no defaults. A name that is unknown,
duplicated or left out is a compile error.

---

## 10. Enums

Enums define a set of named integer constants. Variants are assigned indices in declaration order starting from 0.

```aev
enum Color { red, green, blue }
enum Direction { north, south, east, west }

let c = Color.red;
print c;                    // 0
print c == Color.green;     // false

if (c == Color.red) {
    print "facing red";
}
```

Enum values are `i32` under the hood. They can be compared, printed (prints the variant name), and used in `if` conditions. Assigning to an enum variant or passing one as a `ref` parameter is a compile error.

---

## 11. Epochs (arena rollback)

`epoch { ... }` saves the arena position on entry and restores it on exit, logically freeing everything allocated inside. Variables defined before the epoch survive; slices cannot escape an epoch.

```aev
let keep = new int[2];
epoch {
    let tmp = new int[1000];   // discarded when the epoch exits
}
print len(keep);               // still 2
```

---

## 12. Standard I/O and Builtins

Output is handled by the `print` statement (supports int, float, bool, strings, arrays/slices, structs):
```aev
print "System status: OK";
print 404;
```
`print` accepts multiple comma-separated expressions on one line; values are
space-separated and a single newline is emitted once at the end:
```aev
print 1, "two", 3.5;         // 1 two 3.500000
```
Booleans print as the words `true` / `false` (also inside arrays and structs).
Fixed arrays and open arrays (slices) print in the same bracketed form, for
scalars, strings and structs alike: `[{1, 2}, {3, 4}]`.

Built-in functions:

| Builtin | Signature | Description |
| :--- | :--- | :--- |
| `len(x)` | `len(array or string) -> int` | Length of an array, slice or string |
| `argc()` | `argc() -> int` | Number of CLI arguments (after the program name) |
| `arg(i)` | `arg(i) -> string` | The i-th CLI argument; `""` if out of range |
| `read(fn)` | `read(fn: string) -> string` | Whole file as a string; `""` if unreadable |
| `write(fn, s)` | `write(fn: string, s: string) -> bool` | Write `s` to `fn`, truncating; `true` on success |
| `input()` | `input() -> string` | One line from stdin, without the trailing newline; `""` at EOF |
| `to_int(s)` | `to_int(s: string) -> int` | Leading decimal integer of `s` (whitespace and `-` honored); `0` if none |
| `to_float(s)` | `to_float(s: string) -> float` | Leading floating-point number of `s`; `0.0` if none |
| `to_str(n)` | `to_str(int or float) -> string` | Number rendered as text (floats use `%f`, matching `print`) |
| `substr(s, start, n)` | `substr(s: string, start: int, n: int) -> string` | Clamped segment of `s`, copied to the arena |
| `split(s, sep)` | `split(s: string, sep: string) -> string[]` | Split `s` on every `sep`; empty pieces preserved |
| `join(arr, sep)` | `join(arr: string[], sep: string) -> string` | Join string array with separator |
| `min(a, b)` | `min(int or float...) -> same` | Smaller of two numbers (int/float/mixed) |
| `max(a, b)` | `max(int or float...) -> same` | Larger of two numbers (int/float/mixed) |
| `abs(n)` | `abs(int or float) -> same` | Absolute value |
| `rand(max)` | `rand(max: int) -> int` | Random integer in `[0, max)` |
| `assert(cond)` | `assert(cond: int or bool)` | If `cond` is 0, print error and `exit(1)` |
| `exit(n)` | `exit(n: int)` | Terminate the program with status `n` |

#### String builtins

| Builtin | Signature | Description |
| :--- | :--- | :--- |
| `trim(s)` | `trim(s: string) -> string` | Strip leading/trailing whitespace |
| `upper(s)` | `upper(s: string) -> string` | Uppercase copy |
| `lower(s)` | `lower(s: string) -> string` | Lowercase copy |
| `contains(h, n)` | `contains(h: string, n: string) -> bool` | `true` if `h` contains `n` |
| `startswith(h, p)` | `startswith(h: string, p: string) -> bool` | `true` if `h` starts with `p` |
| `endswith(h, p)` | `endswith(h: string, p: string) -> bool` | `true` if `h` ends with `p` |
| `replace(s, old, new)` | `replace(s: string, old: string, new: string) -> string` | Replace all `old` with `new` |
| `char_at(s, i)` | `char_at(s: string, i: int) -> string` | Single-character slice at index `i` |

```aev
hot {
    print argc();
    if (arg(0) == "fail") { exit(7); }
    let name = input();
    write("out.txt", "hello, " + name);
    print read("out.txt");

    let dice = rand(6) + 1;           // random int 1..6
    assert(dice >= 1);                 // abort if false

    let parts = split("a,b,c", ",");  // ["a", "b", "c"]
    let csv = join(parts, ";");       // "a;b;c"

    let s = "  Hello, World!  ";
    print trim(s);                     // "Hello, World!"
    print upper(s);                    // "  HELLO, WORLD!  "
    print contains(s, "World");        // true
    print replace(s, "World", "Aevix"); // "  Hello, Aevix!  "
    print startswith(s, "  H");        // true
    print endswith(s, "!  ");          // true
    print char_at(s, 2);               // "H"
}
```

---

## 13. The `hot` Region
The `hot` block marks a section as a critical hot path. It is a normal block
scope (variables, statements and loops work inside it), and the compiler
applies cache-oriented optimizations to everything it covers:

- the enclosing function gets LLVM's `hot` attribute (aggressive inlining,
  unrolling, vectorization);
- every loop back edge inside the block carries `llvm.loop` metadata enabling
  vectorization (width 4) and unrolling (count 4);
- local slots are 64-byte (cache line) aligned;
- arena allocations made in straight-line code inside the block (concat,
  literals, `new`) are prefetched into L1 before their first use.

Loop bodies deliberately get **no** software prefetch: any call inside a loop
makes LLVM's vectorizer reject that loop, and a vectorized loop streams
through L1 far faster than a prefetch hint could help — sequential access is
already covered by the hardware prefetcher.

The build pipeline runs `opt -O2` between codegen and `llc`, which is what
actually consumes the `hot` attribute and the `llvm.loop` metadata; without
that stage the hints would never reach an optimizer.

`epoch` blocks are rejected inside `hot`: rolling the arena back would evict
the very lines the region pins, so sema reports `epoch inside hot region
defeats L1 cache pinning`.

```aev
hot {
    let total = 0;
    for (let i = 0; i < len(data); i += 1) {
        total = total + data[i];
    }
    print total;
}
```
