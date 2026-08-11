# QumirDB

QumirDB is an analytical query engine designed to run the same Parquet-backed
workloads natively and in the browser. Native execution uses the C++ runtime and
scheduler; browser execution uses WebAssembly kernels and OPFS-backed datasets.

The project is built around
[Qumir](https://qumir.dev) ([GitHub](https://github.com/resetius/qumir)) as the
compiler/runtime layer for generated query kernels, and
[Coroio](https://coroio.dev) ([GitHub](https://github.com/resetius/coroio)) for
the coroutine-based web service runtime.

The current workload focus is TPC-H-style columnar execution: parquet scans,
filters, projections, joins, aggregates, sort/top-sort, and scheduler-driven
rowset pipelines.

## Repository Layout

- `qdb/` - engine library: SQL/parser, logical plan, passes, kernel generation,
  execution, scheduler, parquet IO.
- `bin/` - command line tools:
  - `qdb` - native query runner.
  - `qdb_plan_export` - exports plans/runtime bundles for the web UI.
- `service/` - web server and static browser workbench.
- `test/` - C++ unit and regression tests.
- `benchmark/` - TPC-H, TPC-DS, and ClickBench query packs and runners.
- `docs/arch/` - design notes for planner, scheduler, joins, aggregation, and
  kernel dispatch.

## Build

Requirements:

- CMake 3.30+
- C++23 compiler
- LLVM
- Apache Arrow and Parquet development packages discoverable via `pkg-config`
- Readline
- GoogleTest

Configure and build:

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

Useful focused builds:

```bash
cmake --build build --target qdb
cmake --build build --target qdb_plan_export
cmake --build build --target qdb_web
cmake --build build --target test_sort
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

Run one test binary directly:

```bash
./build/test/test_sort
```

## CLI

Run SQL against a directory of `<table>.parquet` files:

```bash
./build/bin/qdb --sql --data /path/to/pq1 -i query.sql
```

Without `-i`, `qdb` starts an interactive SQL prompt. Statements end with `;`.
The prompt supports `\d <table>` for table description.

Common options:

```bash
--format console
--format csv
--format '<escape>csv'
--format null

--scheduler single
--scheduler threaded
--scheduler-workers 8
--scan-tasks 8
--scheduler-counters
--verbose
```

Example:

```bash
./build/bin/qdb \
  --sql \
  --data ~/Projects/tpch/pq1 \
  --scheduler single \
  --format '<escape>csv' \
  -i benchmark/tpch/queries/sql/q1.sql
```

## Web Workbench

Build and start the web server:

```bash
cmake --build build --target qdb_web
./build/service/qdb_web --port 8080 --data /path/to/pq1 --cache /path/to/jit-cache
```

Open:

```text
http://localhost:8080
```

`--data <dir>` registers a server-side dataset. Queries against these datasets
run natively through `qdb` on the server.

`--cache <dir>` enables the symbol-granular wasm object cache used while the
service exports browser execution plans.

The workbench can also run queries in the browser. Browser datasets live in
OPFS. You can create them in the UI by uploading `.parquet` files.

### Downloadable Local Data

Use `--local-data <dir>` to expose a local parquet directory as a downloadable
dataset in the UI:

```bash
./build/service/qdb_web \
  --port 8080 \
  --data ~/Projects/tpch/pq1 \
  --local-data ~/Projects/tpch/pq1
```

`--local-data` does not run queries directly. The UI shows a download action;
pressing it downloads the parquet files from the server into browser OPFS and
creates a normal browser dataset. Download progress is byte-based and downloads
multiple files concurrently. The UI also exposes a delete action for removing
the OPFS copy.

Browser UI state is persisted per saved query and dataset pair. Persisted state
includes graph/plans/details/result rows and the exec plan. Wasm payloads are
not persisted, and result persistence is capped at 1000 data rows plus the
header.

## Plan Export

`qdb_plan_export` is the bridge used by the web workbench. It accepts JSON on
stdin and can emit logical/physical graphs plus browser runtime bundles.

The service calls it internally, but it can also be useful when debugging plan
generation:

```bash
./build/bin/qdb_plan_export \
  --stdin-json --stdout-json --cache /path/to/jit-cache < request.json
```

The cache stores reusable wasm64 objects per cacheable symbol; the
query-specific object and final `.wasm` module are rebuilt for every request.

## TPC-H

The benchmark runner expects a base directory containing `pq<N>/` directories,
for example:

```text
/path/to/tpch/
  pq1/
    lineitem.parquet
    orders.parquet
    ...
  pq10/
    ...
```

Run all discovered scales in s-expression mode:

```bash
benchmark/tpch/run_tpch.sh /path/to/tpch
```

Run SF1 SQL query 21 with the single-threaded scheduler:

```bash
MODE=sql \
QDB_ARGS="--scheduler single" \
benchmark/tpch/run_tpch.sh /path/to/tpch 1 21
```

Run several queries:

```bash
MODE=sql benchmark/tpch/run_tpch.sh /path/to/tpch 1 1,4,21
```

Benchmark logs are written to `./results/` by default. Override with `OUT_DIR`.

## ClickBench

The ClickBench runner expects the official `hits.parquet`, either directly in
the supplied directory or under `pq1/`:

```bash
benchmark/clickbench/run_clickbench.sh /path/to/clickbench
benchmark/clickbench/run_clickbench.sh /path/to/clickbench 0,7,42
```

Use `QDB_ARGS` and `OUT_DIR` as with the other benchmark runners. Queries that
require unsupported string operations are skipped unless
`ALLOW_UNSUPPORTED=1` is set.

## Development Notes

- Prefer changing generated/static kernel code in `qdb/kernel/` and `.oz`
  modules rather than duplicating heavy operator logic in C++ or JavaScript.
- Browser execution should keep data as wasm rowsets as long as possible and
  materialize JavaScript rows only at the final result boundary.
- Web downloads and browser datasets use OPFS; IndexedDB stores only metadata.
- Keep plan/runtime changes covered by focused tests in `test/` and, when
  relevant, TPC-H smoke runs through `benchmark/tpch/run_tpch.sh`.
