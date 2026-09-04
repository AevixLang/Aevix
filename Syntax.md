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
| `string` | UTF-8 String | `i8*` |

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

## 6. Special Constructs

### The `hot` Region
The `hot` block designates a critical section for maximum hardware optimization. The compiler will attempt to pin all variables in this block to L1 cache or registers.

```aev
hot {
    // This block is treated as a high-performance hot path
    for (let i = 0; i < 1000000; i = i + 1) {
        process(i);
    }
}
```

---

## 7. Standard I/O
Basic output is handled by the `print` statement.
```aev
print "System status: OK";
print 404;
```
