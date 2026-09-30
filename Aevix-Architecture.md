# AEVIX SYSTEM ARCHITECTURE

This document outlines the internal design and technical implementation of the Aevix compiler pipeline.

---

## System Overview

Aevix employs a multi-stage, multi-language pipeline to balance rapid prototyping of language features with extreme backend performance.

**High-Level Data Flow:**
`Source (.aev)` → `Frontend (C++)` → `AST (JSON v4)` → `Sema (C++)` → `Codegen (C++/LLVM)` → `LLVM IR` → `Machine Code`

---

## Component Breakdown

### 1. The Frontend (C++ with Python fallback)

The frontend transforms human-readable source into a typed JSON AST.

- **Lexer**: Hand-written tokenizer (`frontend/cpp/src/lexer.hpp`) supporting all keywords, operators, and literals.
- **Parser**: Recursive descent parser (`frontend/cpp/src/parser.hpp`) producing a variant-based AST (`ast.hpp`).
- **Import resolver**: Recursive module loading with cycle detection, `AEVIX_PATH` support, and `aevix.lock` package resolution (`importer.hpp`).
- **JSON serializer**: Outputs `ast.json` with `SCHEMA_VERSION = 4` (`json_out.hpp`).

**Rationale for C++:**
20x faster than the Python frontend. Eliminates the Python dependency for the common build path.

**Python fallback:**
The legacy Python+Lark frontend (`frontend/src/`) is still available as a fallback if the C++ binary is not built. It uses the Lark EBNF grammar (`frontend/grammar/aevix.lark`) and produces identical JSON output.

### 2. Static Analysis (Sema)

The semantic analyzer validates the AST before code generation.

- **Type checking**: Width-aware comparisons, operator type validation, fixed-width conversion rules.
- **Scope checking**: Variable/function lookup by ID, redeclaration rejection.
- **Epoch escape analysis**: Prevents arena-allocated values from outliving their epoch.
- **Reference parameter validation**: Lvalue requirement, aliasing rejection, single-mutable-ref rule.
- **Return statement checks**: Arena-linked values rejected from epoch exit.
- **Assignment compatibility**: Type mismatch detection, compound assignment validation.

### 3. The Backend (C++ & LLVM)

The backend transforms the validated AST into LLVM IR.

- **JSON reader**: Deserializes the AST into C++ internal representation (`json_reader.cpp`).
- **Code generator**: Uses LLVM IRBuilder to emit optimized IR for all AST nodes (`codegen.cpp`).
- **Runtime**: C library (`aevix_runtime.c`) providing arena allocation, epoch save/restore, bounds checking, I/O, string operations, and builtins.
- **Width-aware codegen**: Generates type-width-appropriate instructions for fixed-width types (i8–u64, f32/f64).

### 4. The Tooling (Go)

A high-level orchestration layer managing the developer experience.

- **CLI**: Single binary (`aevix`) wrapping the full pipeline: frontend → sema → codegen → opt -O2 → llc → clang.
- **Test runner**: Executes the integration test suite with parallel execution and pretty output.
- **Build pipeline**: Manages invocation of the C++ frontend, sema, backend, and linker.

---

## Development Cycle

### Standard Build Pipeline
1. **Parsing**: `aevix-frontend input.aev` → `ast.json`
2. **Analysis**: `aevix-sema ast.json` → validated AST
3. **Generation**: `aevix-backend ast.json` → `output.ll`
4. **Optimization**: `opt -passes=default<O2> output.ll -o output.opt.ll`
5. **Assembly**: `llc output.opt.ll -o output.o`
6. **Linking**: `clang output.o runtime.o -o program`

### Rapid Prototyping Flow
When adding a new language feature:
`Update grammar/lexer` → `Add AST node` → `Implement sema check` → `Implement LLVM codegen` → `Verify with aevix selftest`.

---

## Roadmap & Technical Debt

- [ ] **Full escape analysis**: Param-level escape checking for epochs.
- [x] **Hot region optimization**: `hot` function attribute, loop vectorize/unroll
  metadata, 64-byte aligned slots, hot allocation prefetch, `opt -O2` pipeline
  stage that consumes them (loops vectorize); `epoch` rejected inside.
- [ ] **SOS transformations**: Automatic stack promotion and SIMD conversion.
- [ ] **Context polymorphism**: Per-call-site function variant generation.
- [ ] **Proof-carrying safety**: Compile-time invariant verification.
- [ ] **Namespace support**: Module-scoped names instead of flat global merge.
- [ ] **Named struct literals**: `Point { x: 1, y: 2 }` syntax.
- [ ] **AST interface**: Evaluate Protobuf as JSON replacement for large files.

---

## Technical Specifications

| Component | Technology | Role |
| :--- | :--- | :--- |
| Grammar | Lark (EBNF) | Syntax reference |
| Frontend | C++17 (primary), Python (fallback) | Lexing, parsing, AST generation |
| Intermediate | JSON (schema v4) | Frontend ↔ Backend bridge |
| Static Analysis | C++17 | Type/scope/escape validation |
| Code Generation | C++17 + LLVM | IR generation |
| Runtime | C | Arena, epochs, builtins, I/O |
| Orchestration | Go | CLI & test runner |
