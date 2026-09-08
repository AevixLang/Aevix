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

### Global install
Bootstrap also symlinks the CLI into `~/.local/bin`, so once you have run
`python bootstrap.py` (or `source activate.sh`) the `aevix` command works from
**any** directory — the CLI locates the compiler repo from its own path, and
relative `.aev` paths / program file I/O resolve to your current working
directory:

```bash
aevix run ~/somewhere/else/prog.aev   # works from anywhere
```

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

Program arguments, stdin and file I/O work out of the box:

```aev
hot {
    let who = input();                       // a line from stdin
    if (len(who) == 0) { who = "world"; }    // "" at EOF
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
- Struct literals are positional (`Point { 1, 2 }`), not named.
- Printing an open array (slice) of structs is not supported (fixed arrays of
  structs and slices of strings work).
- `read()` on a missing file returns `""`; `write()` reports failure as `false`.
- `input()` returns `""` at end of input (EOF); lines are returned without the trailing newline.
- `func` declarations may not be nested inside other functions.
- `import` is top-level only; there are no namespaces yet, so imported
  declarations merge globally and a duplicate name across files is an error.
- Package imports (`name:module`) require a project `aevix.lock` — the package
  manager that writes it is still under development.
- The arena grows on demand (chunked virtual memory) up to what the OS will map; it is never freed individually, and epoch blocks roll the allocation point back.

## Acknowledgments

- LLVM for the powerful backend infrastructure
- Lark for the elegant parsing toolkit
- The open-source community for inspiration
