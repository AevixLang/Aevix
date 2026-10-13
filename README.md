# Aevix

**Aevix** is a high-performance systems programming language designed to deliver C/C++-level speed with stronger safety guarantees.

## Philosophy

Aevix aims to push beyond existing languages by combining:

- **Maximum performance** — AOT compilation via LLVM, zero-cost abstractions
- **Memory safety** — Without garbage collection or borrow checker complexity
- **Developer control** — Explicit memory management when needed, safety by default

## Key Features

### Implemented (v0.1)
- **Static type system** — int, float, bool, string, fixed-width types (i8–u64, f32/f64), structs, enums
- **Arena-based memory (Epochs)** — O(1) rollback, no GC, no borrow checker
- **Reference parameters** — `ref` with aliasing and escape safety checks
- **Arrays & slices** — fixed-size arrays (by value) and open slices (arena-allocated)
- **String builtins** — split, join, trim, upper, lower, contains, replace, startswith, endswith, char_at
- **Hot regions** — `hot { }` marks a critical path: `hot` function attribute,
  loop vectorize/unroll metadata, cache-line aligned locals, allocation
  prefetch (`epoch` is rejected inside)
- **Multi-file imports** — with cycle detection and package support
- **Package manager** — `aevix-pm` maintains `aevix.toml` / `aevix.lock`; the
  compiler resolves `package:module` imports against the lock (local `path`
  dependencies)
- **C++ frontend** — 20x faster than the legacy Python frontend

### Roadmap
- **Self-Optimizing Semantics (SOS)** — Compiler-driven SIMD and stack promotion
- **Context Polymorphism** — Function codegen adapts to calling context
- **Proof-Carrying Safety** — Compile-time invariant proofs to strip runtime checks

## Documentation

### 📘 Language Documentation
*For those writing code in Aevix.*
- [Aevix Manifest](Aevix-Manifest.md) — The vision, core pillars, and high-level goals.
- [Language Syntax](Syntax.md) — Complete grammar, type system, and usage guide.

### 🛠 Compiler Documentation
*For those contributing to the Aevix toolchain.*
- [System Architecture](Aevix-Architecture.md) — Technical breakdown of the C++/Go pipeline.

## Project Architecture

```text
aevix/
├── frontend/
│   ├── cpp/             # C++ lexer/parser → typed JSON AST (primary)
│   │   ├── src/         # lexer.hpp, parser.hpp, ast.hpp, importer.hpp, json_out.hpp
│   │   └── build/       # aevix-frontend binary
│   ├── src/             # Python frontend (legacy fallback)
│   │   └── parser.py, transformer.py, ast.py, importer.py, main.py
│   └── grammar/
│       └── aevix.lark   # Lark EBNF grammar (reference)
├── backend/
│   ├── sema/
│   │   └── sema.cpp     # Static analysis (types, scopes, epoch escape)
│   ├── src/
│   │   ├── codegen.cpp  # LLVM IR generation
│   │   ├── json_reader.cpp  # AST deserialization
│   │   └── aevix_runtime.c  # Arena allocator, I/O, builtins
│   └── build/           # aevix-backend, aevix-sema, runtime.o
├── tools/
│   └── cmd/aevix/main.go   # Go CLI orchestrating the pipeline
└── examples/            # 29 sample programs (.aev)
```

## Compiler Pipeline

```text
main.aev ──► [frontend: C++ (or Python fallback)] ──► ast.json (schema v5)
     ──► [sema: C++] ──► static analysis (types, scopes, epoch safety)
     ──► [backend: C++ + LLVM] ──► output.ll (LLVM IR)
     ──► [opt -O2] ──► output.opt.ll (optimized IR, consumes `hot` metadata)
     ──► [llc] ──► output.o
     ──► [clang + runtime.o] ──► program (executable)
```

## Getting Started

### Prerequisites
- C++ compiler with C++17 support (clang++ or g++)
- **LLVM** (Must be in your system PATH)
- Go 1.25+
- Python 3.8+ (only needed for the legacy frontend fallback)

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
./aevix selftest

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
- `func` declarations are allowed only at the top level of a file.
- `import` is top-level only; no namespaces — imported declarations merge globally.
- Package imports (`name:module`) resolve against a project `aevix.lock` authored by `aevix-pm`. Local `path` dependencies are supported; version-based registry fetching is not yet.
- The arena grows on demand but is never freed individually; epoch blocks roll the allocation point back.
- `hot` accelerates loops (cache-line aligned slots, vectorize/unroll
  metadata consumed by the pipeline's `opt -O2` stage, allocation prefetch)
  but register allocation inside it is still left to LLVM.
- No SOS (Self-Optimizing Semantics) or context polymorphism yet.

## Acknowledgments

- LLVM for the powerful backend infrastructure
- Lark for the elegant parsing toolkit (grammar reference)
- The open-source community for inspiration
