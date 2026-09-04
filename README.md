# Aevix

**Aevix** is a high-performance systems programming language designed to deliver C/C++-level speed with stronger safety guarantees.

## Philosophy

Aevix aims to push beyond existing languages by combining:

- **Maximum performance** — AOT compilation via LLVM, zero-cost abstractions
- **Memory safety** — Without garbage collection or borrow checker complexity
- **Developer control** — Explicit memory management when needed, safety by default

## Key Features (Roadmap)

- **Self-Optimizing Semantics (SOS)** — Compiler rewrites code for optimal execution
- **Cache-Oblivious Hot Path** — Deterministic L1 cache optimization (`hot` regions)
- **Time-Travel Memory (Epochs)** — Arena-based memory with zero overhead
- **Context Polymorphism** — Code adapts to calling context
- **Proof-Carrying Safety** — Compiler-verified invariants

## Documentation

### 📘 Language Documentation
*For those writing code in Aevix.*
- [Aevix Manifest](Aevix-Manifest.md) — The vision, core pillars, and high-level goals.
- [Language Syntax](Syntax.md) — Complete grammar, type system, and usage guide.

### 🛠 Compiler Documentation
*For those contributing to the Aevix toolchain.*
- [System Architecture](Aevix-Architecture.md) — Technical breakdown of the Python/C++/Go pipeline.

## Project Architecture

```text
aevix/
├── frontend/            # Python parser (Lark) -> typed JSON AST
│   ├── grammar/aevix.lark
│   └── src/             # parser.py, transformer.py, ast.py, main.py
├── backend/             # C++ code generator (LLVM)
│   ├── include/         # json_reader.hpp, codegen.hpp
│   └── src/             # json_reader.cpp, codegen.cpp, main.cpp
├── tools/               # Go CLI orchestrating the whole pipeline
│   └── cmd/aevix/main.go
└── examples/            # Sample programs (.aev)
```

## Compiler Pipeline

```text
main.aev ──► [frontend: Python + Lark] ──► ast.json
     ──► [backend: C++] ──► output.ll (LLVM IR)
     ──► [llc] ──► output.o
     ──► [clang] ──► program (executable)
```

## Getting Started

### Prerequisites
- Python 3.8+
- C++ compiler with C++17 support
- **LLVM** (Must be in your system PATH)
- Go 1.25+

### Setup
Instead of manual configuration, use the bootstrap script. It will set up the Python virtual environment, build the C++ backend, and compile the Go CLI.

```bash
python bootstrap.py
```

### Using the Compiler
Once bootstrapped, you can use the `aevix` CLI to build and run your programs:

```bash
# Build a program
./aevix build examples/test.aev

# Build and run a program
./aevix run examples/test.aev

# Build, run, and forward program arguments (argc()/arg(i))
./aevix run examples/test.aev alpha beta

# Run the full integration test suite
./aevix test

# Clean artifacts
./aevix clean
```
*On Windows, use `aevix.exe` instead of `./aevix`.*

### Example
```aev
let x = 10;
let y = 20;
let z = x + y;
let w = z * 2;
let n = -w;

hot {
    let t = n + 1;
    print t;
}
```

Program arguments and file I/O work out of the box:

```aev
hot {
    let who = arg(0);
    if (argc() == 0) { who = "world"; }
    let msg = "hello, " + who + "!";
    write("out.txt", msg);
    print read("out.txt");
}
```

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## Status

🚧 Alpha — Under active development. Not yet production-ready. Frontend and backend pipeline currently working end-to-end.

### Known limitations
- Arrays of strings (`string[3]`) are not supported yet; strings are slices of `i8`.
- Struct literals are positional (`Point { 1, 2 }`), not named.
- Slices of structs and arrays of structs inside print are not supported (int/float/bool/string print everywhere).
- `read()` on a missing file returns `""`; `write()` reports failure as `false`.
- `func` declarations may not be nested inside other functions.
- The arena grows on demand (chunked virtual memory) up to what the OS will map; it is never freed individually, and epoch blocks roll the allocation point back.

## Acknowledgments

- LLVM for the powerful backend infrastructure
- Lark for the elegant parsing toolkit
- The open-source community for inspiration
