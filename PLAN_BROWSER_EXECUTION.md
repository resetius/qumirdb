# Browser Execution Plan

Goal: execute **simple one-to-one pipelines** (`source → filter → project → limit`) entirely
in the browser, using the WASM kernels already compiled by `qdb_plan_export`
(`options.embedWasm = true`), reading parquet data client-side, and rendering the
result — without a server round-trip for `/api/run`.

This document captures the **verified ABI/layout facts** (established by running the
real compiled kernels in Node) plus the implementation plan. Everything under
"Verified facts" was proven empirically, not assumed.

## Status — IMPLEMENTED and verified end-to-end

Numeric filter/project/limit and string **passthrough** run in the browser path.
Verified with a real snappy-compressed parquet file: the browser path
(parquet → exec plan → wasm kernels → limit) produces rows **identical** to native
`qdb` on the same query + data (checked for a numeric filter+project+limit and a
string-passthrough+filter+limit query).

Landed:

- **C++ exporter** (`bin/plan_export.cpp`): emits `bundle.exec` — layout
  constants + typed per-stage plan; compiles filter/project kernels straight from
  the in-memory AST via `CompileKernelAstToWasm` (no text round-trip). Reports
  `supported:false` for anything outside a numeric/string-passthrough linear
  `source/filter/project/limit` chain (incl. string predicates / computed string
  columns — see constraint below).
- **Kernel generator** (`qdb/kernel/gen.cpp`): filter kernels now materialize only
  the columns the predicate reads (`FindUnboundVars`), so a passthrough string
  column is never built into the kernel. This fixes a wasm -O3 miscompile and cuts
  wasted work natively — see
  [docs/issues/qumir_wasm_o3_string_kernel_miscompile.md](docs/issues/qumir_wasm_o3_string_kernel_miscompile.md).
- **JS**: `browser_parquet.js` (`readParquetColumns` via hyparquet + compressors),
  `browser_runtime.js` (marshalling + kernel driver + `executeBrowserPipeline`),
  `browser_storage.js` (`readOpfsFile`), `app.js` (`runBrowser` wired to Run for
  browser datasets).

Constraint (MVP): a filter predicate or computed projection that **reads** a
string column, or a projection that **outputs** a string, is rejected
(`supported:false`) — those need the string inside the kernel, which currently
miscompiles on wasm -O3. Pure string **passthrough** works.

Two qumir bugs found and documented along the way:
[empty-named type printing](docs/issues/qumir_ast_printer_empty_named_type.md),
[native pointer width + pointer cast](docs/issues/qumir_pointer_width_and_pointer_cast.md),
[wasm -O3 string-kernel miscompile](docs/issues/qumir_wasm_o3_string_kernel_miscompile.md).

---

## Current state (branch `browser-execution`)

Already done:

- **UI** (`service/static/`): editor, dataset drawer, graph view, inspector.
- **Browser datasets**: parquet files stored in OPFS (`browser_storage.js`);
  only parquet **metadata** (schema + row-group stats) is read so far
  (`browser_parquet.js` → `readParquetTable`), not the actual row data.
- **Plan export** (`bin/plan_export.cpp`): SQL → optimized logical plan → scheduler
  graph, emitted as a `qdb.runtime.bundle` JSON. With `embedWasm=true` each
  filter/project kernel is compiled to a standalone `.wasm` and base64-embedded in
  `bundle.artifacts` (kind `"wasm"`, keyed to a node via `taskGroup`/stage).
- **Single-thread lowering**: `options.scheduler="single"` + `scanTasks=1` produces a
  clean linear **one-to-one** chain `source → filter → project → limit`
  (connections all `kind:"one-to-one"`). This is the shape we execute.
- **Kernel export hook**: `IKernelExportBackend` (`qdb/kernel/compiler.h`) lets the
  exporter capture the generated kernel AST and compile it to WASM
  (`TWasmKernelExportBackend` in `plan_export.cpp`, `CompileKernelSourceToWasm`).

Done in this task (see Status above):

- Typed **execution plan** in the bundle (`bundle.exec`).
- Reading actual parquet **row data** in the browser (`readParquetColumns`).
- A **JS WASM runtime** that marshals batches and drives the kernels.
- Run button wired to browser execution for browser datasets.

Not verified in-environment (browser-only glue, syntactically checked, mirrors the
verified data flow): OPFS file read + DOM result rendering. Worth a manual smoke
test in a real browser.

---

## MVP scope (decided)

**Numbers + string passthrough.**

- Numeric columns (`i8/i16/i32/i64/u*/f64/bool/date`): fully supported — used in
  filters and in computed `project` expressions.
- String columns: flow through `source → project → limit` (passthrough, displayed),
  **but** filters/computed expressions over strings are deferred (they need the
  string `extern` functions — see below — which numeric kernels don't import).

---

## Verified facts (proven by running real kernels in Node)

### Kernel modules

Compiled numeric `filter`/`project` kernels are **fully self-contained**:

- **No imports at all** (`WebAssembly.Module.imports(module) === []`). The caller
  supplies the `Selection` buffer (filter) and the output buffers (project), so the
  kernels never call `qdb_alloc`/`qdb_free`/etc. → JS needs to provide **no** env
  functions for the numeric MVP.
- Exports include `memory` (the module owns its linear memory), the entry function
  under its literal name **`"<kernel>"`** (filter) / **`"<project>"`** (project),
  `__wasm_call_ctors`, and globals incl. **`__heap_base`** (first free byte — use as
  the bump-allocator start).
- Each kernel is a **separate module with its own memory**. The JS orchestrator
  marshals data into each module's memory per call (no shared memory across modules
  in the MVP).

### Struct layout — wasm32, **8-byte pointers**

Critical discovery: **qumir models pointers as 64-bit (8 bytes) even on
`wasm32-unknown-unknown`.** So struct pointer fields occupy 8 bytes and the wasm32
layout **matches the native 64-bit layout**. Pointer values are stored little-endian
with the address in the low 32 bits (high 32 bits zero).

`TColumn` — size **48**, align 8:

| field          | type      | offset |
|----------------|-----------|--------|
| Data           | ptr (8)   | 0      |
| DataBitOffset  | i32       | 8      |
| Mask           | ptr (8)   | 16     |
| MaskBitOffset  | i32       | 24     |
| Offsets        | ptr (8)   | 32     |
| OffsetWidth    | u8        | 40     |

`TRowSet` — size **56**, align 8:

| field       | type    | offset |
|-------------|---------|--------|
| Columns     | ptr (8) | 0      |
| ColumnCount | i64     | 8      |
| RowCount    | i64     | 16     |
| Selection   | ptr (8) | 24     |
| Destroy     | ptr (8) | 32     |
| Private     | ptr (8) | 40     |
| RefCount    | i64     | 48     |

`StringView`/`OwnedString` — size **16**: `Data` ptr(8) @0, `Size` i64 @8.

Columns are indexed contiguously: `cols[k]` is at `Columns + k * 48`.

### Kernel calling convention

- **filter**: `kernel(rowSetPtr: i32)`.
  - Caller sets `RowCount`, `Columns` (ptr to a contiguous `TColumn[ColumnCount]`),
    and `Selection` (ptr to a `u8[RowCount]` buffer the caller owns).
  - Kernel reads each referenced column's `Data`, casts to the typed pointer, and
    writes `Selection[i]`. **True is written as `0xFF`, not `1`** — treat any
    nonzero as selected.
  - Filter does **not** compact; it only fills `Selection`.
- **project**: `project(rowSetPtr: i32, outPtr: i32)`.
  - `outPtr` points to an array of 8-byte pointers, one per **computed** (non-ident)
    output column, in output order; each points to a caller-owned buffer of
    `RowCount * width` bytes (`width`: i8=1,i16=2,i32/date=4,i64/f64=8,bool=1,
    string=16 (a `StringView` per row)).
  - Project computes over **all `RowCount` rows** and **ignores Selection**.
    Passthrough (ident) columns are **not** in the kernel — the JS orchestrator maps
    them straight from the input column.
  - Column indices in the kernel refer to the **input** rowset column order.

### Orchestration semantics for a `source → filter → project → limit` chain

1. `source`: read the pruned columns (in the plan's column order) from parquet into
   columnar typed arrays; build a `TRowSet`.
2. `filter`: run kernel → `Selection` (nonzero = keep).
3. `project`: run kernel over all rows → computed buffers; passthrough cols reference
   input cols. Selection carries through unchanged.
4. `limit(n, offset)`: while emitting rows, iterate `i` where `Selection[i]` is
   nonzero, skip `offset`, take `n`.

If `project` has **no computed columns** it has no wasm module — pure passthrough.

---

## Why the `(cast (cast Data i64) <ptr T>)` double-cast? (answer to a review question)

The generated kernels read a column as, e.g.:

```
(cast (cast (field col Data) i64) <ptr i64>)
```

- `TColumn.Data` is typed `<ptr i8>` (a byte pointer). The kernel needs a **typed**
  pointer (`<ptr i64>`, `<ptr f64>`, …). qumir core-lang has no direct
  pointer→pointer reinterpret, so the codegen **launders it through an integer**:
  `<ptr i8> → i64 → <ptr T>`. Source: `qdb/kernel/gen.cpp` (`columnData` lambda,
  ~line 1394, and the comment at ~line 794).
- It goes through **`i64`** (not `i32`) because **qumir uses 64-bit pointers
  universally**, so a pointer and `i64` are the same width. That is also why the
  wasm32 struct layout above uses 8-byte pointer fields.

Verdict: **not a bug.** It's a deliberate reinterpret idiom in qdb's kernel
generator. On the native 64-bit JIT it is a free no-op. On wasm32 it costs a
harmless zero-extend/truncate. A bonus: because pointers are 64-bit everywhere, the
**same struct offsets work for native and wasm** — no per-target layout needed. (If
we ever want leaner wasm we could add a direct pointer-cast to qumir and drop the
i64 hop, but there is no correctness reason to.)

---

## Implementation plan

### Part A — C++: emit a typed execution plan in the bundle

Add an `"exec"` object to the bundle, produced only when the plan is a supported
linear one-to-one chain of `{source, filter, project, limit}` (else
`{"supported": false, "reason": ...}` and JS falls back).

Shape:

```json
"exec": {
  "supported": true,
  "layout": { "pointerSize": 8,
    "column": {"size":48,"data":0,"mask":16,"offsets":32,"offsetWidth":40},
    "rowset": {"size":56,"columns":0,"columnCount":8,"rowCount":16,"selection":24},
    "stringView": {"size":16,"data":0,"size_":8} },
  "stages": [
    {"kind":"source","table":"lineitem",
     "columns":[{"name":"l_orderkey","type":"i64","nullable":true}, ...]},
    {"kind":"filter","wasm":"a5"},
    {"kind":"project","wasm":"a7",
     "output":[
       {"name":"l_orderkey","source":"passthrough","inputIndex":0,"type":"i64"},
       {"name":"q","source":"computed","computedIndex":0,"type":"i64",
        "width":8,"isString":false}]},
    {"kind":"limit","limit":5,"offset":0}
  ]
}
```

- Walk the optimized typed plan `*plan` bottom-up (each op has exactly one child in a
  linear chain). Thread the input type: `BuildSourceRuntimeType` gives the source
  output schema (and applies column pruning); it becomes the filter/project input.
- **source.columns**: bare parquet names (strip `alias.` prefix) + core type +
  nullable, in the source struct field order (this is the rowset column order the
  kernels index into).
- **project.output**: replicate the ident-vs-computed split from
  `BuildProjectRuntimeProcess` (ident → passthrough with `inputIndex`; else computed
  with running `computedIndex`, `width` from output type, `isString`). Types from the
  project `OutputType` struct fields (same order as projections).
- **wasm ids**: compile each filter/project kernel through a small per-stage
  `IKernelExportBackend` that captures the single kernel's WASM and registers it in
  the existing `TArtifactStore` (reuse `CompileKernelSourceToWasm`). Only when
  `embedWasm` is set.
- Emit `layout` constants (above) so JS never hardcodes offsets.
- Type-name mapping: unwrap nullable/named → `i8/i16/i32/i64/u8.../f64/bool/string`
  (dates are `i32`), plus a `nullable` flag.

### Part B — JS: read parquet row data

Extend `browser_parquet.js` with a `readParquetColumns(file, columnNames)` that uses
hyparquet's data API (`parquetReadObjects`/`parquetRead`) to return columnar typed
arrays for the requested columns. Convert to the kernel value representation
(i64 → BigInt64Array; dates → i32 days; strings → {bytes, offsets}).

### Part C — JS: WASM runtime driver (`service/static/browser_runtime.js`)

- `instantiateKernel(base64Wasm)` → `{instance, memory, heapBase, fn}` (fn from
  `exports["<kernel>"]` or `exports["<project>"]`); grow memory as needed; simple
  bump allocator from `__heap_base` (pattern mirrors
  `externals/qumir/service/static/runtime/array.js`).
- `writeColumn` / `writeRowSet` per the verified layout (8-byte pointers).
- `runFilter(stage, batch)` → selection (nonzero = keep).
- `runProject(stage, batch)` → new columns (computed buffers + passthrough refs).
- `applyLimit(rows, selection, limit, offset)` and a `rowsToCsv`/rows accessor for
  the UI.

### Part D — JS: pipeline interpreter + wiring (`app.js`)

- `executeBrowser(bundle.exec, dataset)`: source (read parquet cols) → filter →
  project → limit; produce `{columns, rows}`.
- In the Run handler for `dataset.source.kind === 'browser'`: call `explain` with
  `scheduler:'single', scanTasks:1, embedWasm:true`, then run `executeBrowser`,
  then `renderResult`. If `exec.supported === false`, show the reason.

### Verification

- Native reference: `build/bin/qdb` on the same SQL + a small parquet → compare rows.
- Node harness already validated filter+project layout end-to-end; extend it to run a
  full `source→filter→project→limit` chain against a fixture before UI wiring.

---

## Deferred (post-MVP)

- String filters / computed string expressions (needs JS impls of the `qdb_*` string
  externs: `qdb_filter_string_compare`, `qdb_string_view_sql_like`, `qdb_substring`,
  `qdb_sql_bool_*`, `qdb_date_*`, and `StringView` marshalling).
- Nullable masks in filter/project (bitmap read/write; `qdb_bitmap_set_valid`).
- Aggregate / join / sort / top-sort operators (stateful; need the opaque-handle ABI
  from `PLAN_WASM_BROWSER_RUNTIME.md`).
- Parallel graphs (gather / hash-shuffle); multi-lane execution; Web Workers.
- Shared single WASM runtime memory for the whole pipeline (avoid per-stage copies).
