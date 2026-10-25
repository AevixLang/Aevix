# Changelog

All notable changes to Aevix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) principles and the
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Import alias parsing** — `import "mod.aev" as mod;` now parses in both
  frontends and the alias is carried in the AST (`Import.alias`, emitted as
  `"alias"` in JSON). This is step 1 of namespaces; qualified access
  (`mod.fn()`) is not yet resolved by sema/codegen.

## [0.3.1] — 2026-10-24

### Added
- **`std:collections`** — `sort_int` (in-place), `reverse_int`, `index_of_int`,
  `contains_int`, `min_int`, `max_int`, `sum_int`.
- **SemVer ranges in `aevix-pm`** — `add --version` validates `^`, `~`, `=`,
  `>=`, `<=`, `<`, `>` and comma-separated ranges.

### Fixed
- **Short-circuit `&&` / `||`** — the result phi recorded the wrong predecessor
  block, so an operand evaluated in a nested block (e.g. `while (j >= 0 && arr[j] > v)`)
  read an undef value. Issue #12.
- **`assert()`** — accepts `bool` conditions in addition to integers.

## [0.3.0] — 2026-10-24

### Added
- **Standard library (`std:`)** — `std:text`, `std:io`, `std:math` ship *with*
  the toolchain and bypass `aevix.lock`. `import "std:"` loads the full surface;
  individual modules available as `std:text` etc.
- **`%` modulo operator** — full support (grammar, both frontends, sema, codegen).
- **`match` statement** — `match (x) { case V: { ... } default: { ... } }`, lowered
  to an if-chain; enums require exhaustive coverage or a `default` (checked by
  sema) and duplicate cases are rejected.
- **Float math builtins** — `sqrt`, `sin`, `cos`, `tan`, `log`, `exp`, `floor`,
  `ceil`, `pow` (LLVM intrinsics on top of the runtime).

### Changed
- **String predicates return `bool`** — `contains`, `startswith`, `endswith`
  are now typed as `bool` (docs already claimed so); codegen truncates the
  runtime's i32 to i1.

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
