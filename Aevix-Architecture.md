# AEVIX SYSTEM ARCHITECTURE

This document outlines the internal design and technical implementation of the Aevix compiler pipeline.

---

## 🗺 System Overview
Aevix employs a multi-stage, multi-language pipeline to balance rapid prototyping of language features with extreme backend performance.

**High-Level Data Flow:**
`Source (.aev)` $\rightarrow$ `Frontend (Python)` $\rightarrow$ `AST (JSON)` $\rightarrow$ `Backend (C++)` $\rightarrow$ `LLVM IR (.ll)` $\rightarrow$ `Machine Code (.o)`

---

## 🏗 Component Breakdown

### 1. The Frontend (Python & Lark)
The frontend is responsible for the transition from human-readable text to a structured tree.

- **Lexer/Parser**: Powered by **Lark**, utilizing an EBNF grammar (`aevix.lark`).
- **Transformer**: Converts the raw parse tree into a strongly-typed AST using Python dataclasses.
- **Serialization**: The AST is exported to a JSON format, serving as the universal interface between the frontend and backend.

**Rationale for Python:** 
Allows for "rapid-fire" syntax iteration. Adding a new keyword or operator takes minutes and requires no recompilation of the frontend itself.

### 2. The Backend (C++ & LLVM)
The backend transforms the AST into high-performance machine code.

- **JSON Reader**: Deserializes the AST and rebuilds the internal C++ representation.
- **Semantic Analyzer**: Validates types, resolves variable scopes, and ensures Epoch constraints are met.
- **Code Generator**: Utilizes the **LLVM IRBuilder** to emit optimized intermediate representation.
- **Optimization Pass**: Implements the "SOS" (Self-Optimizing Semantics) and `hot` region constraints.

**Rationale for C++:** 
Provides the lowest-level access to LLVM's API and ensures that the compiler itself remains performant, especially during heavy optimization passes.

### 3. The Tooling (Go)
A high-level orchestration layer that manages the developer experience.

- **CLI**: A single binary (`aevix`) that wraps the frontend and backend calls.
- **Test Runner**: Executes a suite of `.aev` tests in parallel across multiple CPU cores.
- **Build Pipeline**: Manages the invocation of `python` $\rightarrow$ `aevix-backend` $\rightarrow$ `clang/gcc`.

**Rationale for Go:** 
Offers superior concurrency for testing and fast compilation of the toolchain, providing a "snappy" experience for the end-user.

---

## 🔄 Development Cycle

### Standard Build Pipeline
1. **Parsing**: `python -m src.main input.aev` (from `frontend/`) $\rightarrow$ `ast.json`
2. **Generation**: `./backend/build/aevix-backend ast.json` $\rightarrow$ `output.ll`
3. **Assembly**: `llc output.ll -o output.o`
4. **Linking**: `clang output.o -o program`

### Rapid Prototyping Flow
When adding a new language feature:
`Update .lark grammar` $\rightarrow$ `Add AST node in ast.py` $\rightarrow$ `Implement LLVM logic in codegen.cpp` $\rightarrow$ `Verify with aevix test`.

---

## 🚀 Roadmap & Technical Debt
- [ ] **AST Interface**: Transition from JSON to Protobuf for improved performance on large files.
- [ ] **Semantic Layer**: Implement a full-scale type-checker in `semantic.cpp`.
- [ ] **SOS Implementation**: Begin implementing the laziest-evaluated SIMD transformations.
- [ ] **Epochs**: Introduce arena-based allocation in the LLVM backend.

---

## 🛠 Technical Specifications
| Component | Technology | Role |
| :--- | :--- | :--- |
| Grammar | Lark (EBNF) | Syntax Definition |
| Intermediate | JSON | Frontend $\leftrightarrow$ Backend Bridge |
| IR Generation | LLVM | Code Generation |
| Orchestration | Go | CLI & Tooling |
