# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`qumirdb` (`qdb`) is an early-stage query engine / database layer built on top of **Qumir** — a tiny experimental programming language with Russian keywords (inspired by KUMIR / Ershov). The `qumir` language toolchain lives as a git submodule under `externals/qumir/`.

## Build

Prerequisites: CMake ≥ 3.30, Clang/GCC with C++23, LLVM ≥ 20, Ninja (recommended).

```bash
# Configure (already done; build/ exists)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build -j

# Run tests
cd build && ctest --output-on-failure

# Build with service component (disabled by default)
cmake -S . -B build -DBUILD_SERVICE=ON
```

The main output is `build/bin/qdb` (CLI entry point).

## Architecture

```
qdb/          — core library target: qumirdb (C++23, links qumir)
  io/io.h     — columnar I/O abstractions (TColumn, TRowSet, ISource, ISink)
  stub.cpp    — placeholder implementation
bin/
  cli.cpp     — qdb CLI executable, links qumirdb
test/         — test suite (ctest)
externals/
  qumir/      — Qumir language submodule: parser → IR → LLVM codegen/JIT
```

**Namespace:** `NQdb` (all qdb code lives here).

**Data model:** columnar — `TColumn` holds a data buffer, a null mask, and an offsets array (for variable-length types). `TRowSet` groups columns with a row count. `ISource`/`ISink` are the streaming pull/push interfaces.

**Qumir submodule** provides the language runtime that qdb will use as its query language. Key qumir internals: `qumir/parser/`, `qumir/ir/`, `qumir/codegen/llvm/`, `qumir/runtime/`, `qumir/runner/`.

## Query pipeline

SQL is built into a logical plan, rewritten by a sequence of idempotent passes, then physically planned. Entry point: `bin/cli.cpp` (`RunQuery`); builder `qdb/plan/build.cpp`; passes `qdb/plan/passes/`.

1. **Build** — SQL AST → naive operator tree (no keys, no pushdown, no reorder). See [docs/arch/logical_plan_build.md](docs/arch/logical_plan_build.md) and [docs/arch/decorrelation.md](docs/arch/decorrelation.md).
2. **AssignSourceAliases** — unique alias per source.
3. **QualifyColumns** — rewrite refs to `alias.col`; set source/join schemas.
4. **AnnotateTypes** — attach `TFunctionType` (input/output schema) to every op; idempotent, re-run after each structural rewrite.
5. **ReorderJoins** — cross-join chains → connected order. See [docs/arch/join_reorder.md](docs/arch/join_reorder.md).
6. **ExtractEquiJoins** — lift equi-keys (equivalence classes), push predicates per side, leave residuals. See [docs/arch/predicate_pushdown.md](docs/arch/predicate_pushdown.md).
7. **ApplyColumnPruning** — narrow each op's required input columns.

## Code style

See externals/qumir/docs/arch/codestyle.md 

- C++23 throughout.
- Clang-based toolchain (`/opt/homebrew/bin/clang++`, LLVM 22).
- `compile_commands.json` is generated into `build/` and is kept up to date automatically by CMake (`CMAKE_EXPORT_COMPILE_COMMANDS ON`).
