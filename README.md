# Velo

**Velo** is a high-performance systems programming language designed to deliver C/C++-level speed with stronger safety guarantees.

## Philosophy

Velo aims to push beyond existing languages by combining:

- **Maximum performance** — AOT compilation via LLVM, zero-cost abstractions
- **Memory safety** — Without garbage collection or borrow checker complexity
- **Developer control** — Explicit memory management when needed, safety by default

## Key Features (Roadmap)

- **Self-Optimizing Semantics (SOS)** — Compiler rewrites code for optimal execution
- **Cache-Oblivious Hot Path** — Deterministic L1 cache optimization
- **Time-Travel Memory (Epochs)** — Arena-based memory with zero overhead
- **Context Polymorphism** — Code adapts to calling context
- **Proof-Carrying Safety** — Compiler-verified invariants

## Project Architecture
```text
velo/
├── frontend/ # Python parser with Lark
├── backend/ # C++ code generator with LLVM
├── tools/ # Go CLI tools
└── examples/ # Sample programs
```

## Getting Started

### Prerequisites

- Python 3.8+
- C++ compiler with C++17 support
- LLVM 14+
- Go 1.21+

### Setup

```bash
# Clone the repository
git clone https://github.com/yourusername/velo.git
cd velo

# Setup Python environment
make setup

# Activate virtual environment
./activate.sh

# Build the compiler
make build

# Run an example
make run
```

### Example

```velo
let x = 10;
let y = 20;

hot {
    let z = x + y;
    return z;
}
```

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

🚧 Alpha — Under active development. Not yet production-ready.

## Acknowledgments

- LLVM for the powerful backend infrastructure
- Lark for the elegant parsing toolkit
- The open-source community for inspiration
