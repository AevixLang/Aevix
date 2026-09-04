# Aevix Language Syntax

This document provides the complete specification for the Aevix language (v0.1).

## ⚡ Quick Start
```aev
func calculate_sum(limit: int) : int {
    let total = 0;
    for (let i = 0; i < limit; i = i + 1) {
        total = total + i;
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
| `float` | Double-precision float | `double` |
| `bool` | Boolean (`true` / `false`) | `i1` |
| `string` | UTF-8 string slice (`{ i8*, i32 }`) | `{ i8*, i32 }` |

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

### Iteration (`while`)
```aev
while (condition) {
    // body
}
```

### Iteration (`for`)
C-style loop: `for (init; condition; step) { body }`
```aev
for (let i = 0; i < 10; i = i + 1) {
    print i;
}
```

### Iteration (`for-in`)
Iterate over array elements: `for var in array { body }`
```aev
let nums = [10, 20, 30];
for x in nums {
    print x;
}
```

---

## 5. Functions
Functions are defined using the `func` keyword. The return type is optional (defaults to `void`).

```aev
func power(base: float, exp: float) : float {
    return base * exp; // Simplified for example
}
```

---

## 6. Arrays and Slices

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

## 7. Strings

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

Strings work as function parameters/returns and struct fields. Arrays of strings are not supported yet.

---

## 8. Structs

```aev
struct Point { x: int, y: int }
struct Bag { items: int[], tag: string }

let p = Point { 1, 2 };             // positional literal
print p.x + p.y;                    // 3

let b = Bag { new int[3], "box" };  // open-array fields allowed
b.items[1] = 55;
print b;                            // {[0, 55, 0], box}
```

Struct fields are accessed with `.`; member access chains and arrays of structs work. Struct literals are positional (named fields are not supported yet).

---

## 9. Epochs (arena rollback)

`epoch { ... }` saves the arena position on entry and restores it on exit, logically freeing everything allocated inside. Variables defined before the epoch survive; slices cannot escape an epoch.

```aev
let keep = new int[2];
epoch {
    let tmp = new int[1000];   // discarded when the epoch exits
}
print len(keep);               // still 2
```

---

## 10. Standard I/O and Builtins

Output is handled by the `print` statement (supports int, float, bool, strings, arrays/slices, structs):
```aev
print "System status: OK";
print 404;
```

Built-in functions:

| Builtin | Signature | Description |
| :--- | :--- | :--- |
| `len(x)` | `len(array or string) -> int` | Length of an array, slice or string |
| `argc()` | `argc() -> int` | Number of CLI arguments (after the program name) |
| `arg(i)` | `arg(i) -> string` | The i-th CLI argument; `""` if out of range |
| `read(fn)` | `read(fn: string) -> string` | Whole file as a string; `""` if unreadable |
| `write(fn, s)` | `write(fn: string, s: string) -> bool` | Write `s` to `fn`, truncating; `true` on success |
| `input()` | `input() -> string` | One line from stdin, without the trailing newline; `""` at EOF |
| `exit(n)` | `exit(n: int)` | Terminate the program with status `n` |

```aev
hot {
    print argc();
    if (arg(0) == "fail") { exit(7); }
    let name = input();          // read a line from the terminal or a pipe
    write("out.txt", "hello, " + name);
    print read("out.txt");
}
```

---

## 11. The `hot` Region
The `hot` block designates a critical section for maximum hardware optimization. The compiler will attempt to pin all variables in this block to L1 cache or registers.

```aev
hot {
    // This block is treated as a high-performance hot path
    for (let i = 0; i < 1000000; i = i + 1) {
        process(i);
    }
}
```
