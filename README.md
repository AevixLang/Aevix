# Velo

**Velo** is a high-performance systems programming language designed to deliver C/C++-level speed with stronger safety guarantees.

## Philosophy

Velo aims to push beyond existing languages by combining:

- **Maximum performance** — AOT compilation via LLVM, zero-cost abstractions
- **Memory safety** — Without garbage collection or borrow checker complexity
- **Developer control** — Explicit memory management when needed, safety by default

## Key Features (Roadmap)

- **Self-Optimizing Semantics (SOS)** — Compiler rewrites code for optimal execution
- **Cache-Oblivious Hot Path** — Deterministic L1 cache optimization (`hot` regions)
- **Time-Travel Memory (Epochs)** — Arena-based memory with zero overhead
- **Context Polymorphism** — Code adapts to calling context
- **Proof-Carrying Safety** — Compiler-verified invariants

## Project Architecture

```text
velo/
├── frontend/            # Python parser (Lark) -> typed JSON AST
│   ├── grammar/velo.lark
│   └── src/             # parser.py, transformer.py, ast.py, main.py
├── backend/             # C++ code generator (LLVM)
│   ├── include/         # json_reader.hpp, codegen.hpp
│   └── src/             # json_reader.cpp, codegen.cpp, main.cpp
├── tools/               # Go CLI orchestrating the whole pipeline
│   └── cmd/velo/main.go
└── examples/            # Sample programs (.velo)
```

## Compiler Pipeline

```text
main.velo ──► [frontend: Python + Lark] ──► ast.json
     ──► [backend: C++] ──► output.ll (LLVM IR)
     ──► [llc] ──► output.o
     ──► [clang] ──► program (executable)
```

## Getting Started

### Prerequisites

- Python 3.8+
- C++ compiler with C++17 support
- **LLVM** (installed via Homebrew: `brew install llvm`)
- Go 1.21+

> Note: the Go CLI in `tools/cmd/velo/main.go` hardcodes the LLVM path to
> `/opt/homebrew/opt/llvm/bin`. Adjust the `llvmBin` constant if your install
> differs.

### Setup

```bash
# Setup Python environment (venv + dependencies)
make setup

# Activate virtual environment
./activate.sh

# Build the C++ backend
make build

# Compile + run an example
make run                  # uses examples/test_full.velo by default
make run FILE=examples/test.velo
```

### Example

```velo
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

## Current Syntax Support

- `let <name> = <expr>;` — variable declaration
- `print <expr>;` — print an expression
- `hot { ... }` — hot region block (statements)
- Expressions: numbers, variables, `+ - * /`, unary minus `-x`

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## Status

🚧 Alpha — Under active development. Not yet production-ready. Frontend and backend pipeline currently working end-to-end.

## Acknowledgments

- LLVM for the powerful backend infrastructure
- Lark for the elegant parsing toolkit
- The open-source community for inspiration
