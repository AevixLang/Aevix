# Changelog

All notable changes to Aevix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) principles and the
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] — 2026-10-13

Hardening & memory-safety release. Static checks move into a dedicated `sema`
stage that becomes the single source of truth, and `hot` regions gain real
L1-cache optimizations.

### Added
- **Dedicated sema stage** — type, scope and epoch-escape analysis split out of
  codegen into their own pipeline step (`aevix build --stage sema`).
- **`hot` region L1 optimizations** — LLVM `hot` attribute, loop
  vectorize/unroll metadata, cache-line (64-byte) aligned locals, straight-line
  allocation prefetch, and a new `opt -O2` pipeline stage that consumes the
  metadata. `epoch` inside `hot` is rejected.
- **Fixed-width types** — `i8`–`i64`, `u8`–`u64`, `f32`/`f64` with width-aware
  codegen and enforced conversion rules.
- **Enums** — named integer constants, declaration-order indices, name printing.
- **Named struct literals** — `Point { y: 2, x: 1 }`, any order, all-or-nothing
  (positional and named styles cannot mix). *Bumps AST schema v4 → v5.*
- **String builtins** — `trim`, `contains`, `replace`, `upper`, `lower`,
  `startswith`, `endswith`, `char_at`, plus per-character string indexing.
- **`rand(max)`** and **`assert(cond)`** builtins.
- **C++ frontend** — lexer, parser and import resolver rewritten in C++ (~20x
  faster); the Python frontend remains as a fallback.
- **Diagnostics with real positions** — `line`/`col` from the C++ frontend and
  the true source filename reach the CLI (previously every error showed
  `input.aev` with no position).
- **Golden output corpus and `aevix bench`**, per-source temp build dirs,
  stage timing, and `--stage` flag for running a single pipeline step.

### Changed
- **Escape checks live in sema, not codegen** — sema now models string
  provenance (literal = global constant, concat/builtin result = arena memory),
  so epoch escapes are caught one stage earlier.
- **`ref` params pass by address** with lvalue, initialization and aliasing
  rules; the `out` keyword was dropped — a single `ref` covers in/out.
- **Global `let` is backed by module globals** so function bodies can assign
  file-scope variables (previously a codegen error).
- `aevix test` renamed to **`aevix selftest`** with a formatted test runner.

### Fixed
- **Use-after-free via struct-wrapped slices** — an arena slice nested in a
  struct can no longer escape its epoch (`no-uaf` rule restored).
- **Epoch escape through struct values** — struct variables, struct literals and
  call results returning arena memory are rejected by sema.
- **Array literals of structs** compile in the annotated open-array form
  (`let a: P[] = [P { 1, 2 }];`).
- **Printing an open array of structs** no longer fails at codegen.
- **Nested `func` declarations** are rejected at sema with a clear message
  (functions are top-level only).
- **Missing source file** produces a clear CLI error instead of a crash.

## [0.1.0] — 2026-09-30

First working end-to-end pipeline: source → native binary through LLVM.

### Added
- **Type system** — `int`, `float`, `bool`, `string`, structs.
- **Epoch-based arena memory** — O(1) rollback, escape analysis, no GC.
- **Arrays & slices** — fixed-size (by value) and open (arena-allocated) with
  `new` and runtime bounds checks.
- **Reference parameters** (basic).
- **Multi-file `import`** with cycle detection.
- **`hot` scope marker** (attribute only; no L1 optimizations yet).
- **Go CLI** — `build`, `run`, `selftest`, `bench`, `clean` orchestration.
- **LLVM backend** — IR generation, `llc`, `clang` + runtime linking.

[Unreleased]: https://github.com/aevix-lang/aevix/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/aevix-lang/aevix/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/aevix-lang/aevix/releases/tag/v0.1.0
