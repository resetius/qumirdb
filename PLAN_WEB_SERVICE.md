# Web Service Plan

## Goal

Build a browser UI and a C++ backend for inspecting QumirDB queries:

- left slide-out pane: saved queries;
- center: SQL query editor;
- right slide-out pane: browser-attached datasets;
- bottom: tabular query result area;
- toolbar: `Run` and `Explain`;
- first milestone: `Run` is present but not functional, `Explain` shows the
  query graph;
- graph nodes are clickable and expose AST / IR / LLVM IR;
- graph connections are hoverable/clickable and expose connection kind
  (`one-to-one`, `gather`, `hash-shuffle`, `broadcast`, ...);
- first dataset implementation is a browser-side mock TPC-H dataset that exposes
  schemas only, no data.

## Assumptions

- Lowering stays in C++, but the web service should not do it in-process.
  The service calls a console plan-export tool through `coroio` pipes so heavy
  parse/type/lower/compile work cannot block HTTP I/O.
- The browser owns dataset/schema metadata. This matches the future browser
  execution model where files are attached in browser FS and the browser derives
  or stores schemas next to them.
- Frontend is plain HTML/CSS/JS. CDN dependencies are acceptable, matching the
  qumir service style (`CodeMirror` from `unpkg`, other small libraries from
  CDN if useful).
- Backend is a new qdb service binary using `coroio`, modeled after
  `externals/qumir/service/server.cpp`.
- Do not use qumir's nested `coroio` as an implicit dependency. Add `coroio` as
  a qdb-level external module and link the qdb service to that target.
- First service is local/dev oriented. Authentication, persistence beyond local
  browser storage, uploads of real data, and production deployment are out of
  scope for the first pass.

## Non-goals For The First Pass

- Query execution from the web UI.
- Real parquet data upload / browser-side table data transfer.
- JS runtime over WASM kernels.
- Backend-owned dataset catalog.
- Multi-user saved query storage.
- Graph editing.

## Directory Layout

Proposed layout:

```text
service/
  CMakeLists.txt
  server.cpp
  static/
    index.html
    styles.css
    app.js
    graph.js
    api.js
bin/
  plan_export.cpp    # qdb_plan_export: stdin request -> stdout JSON bundle
```

External dependency:

```text
externals/
  CMakeLists.txt
  coroio/          # added as its own module/submodule/vendor dir
  qumir/
```

If `externals/qumir` is built with `BUILD_SERVICE=ON`, it may also try to add
its nested `externals/coroio`. The qdb build should avoid duplicate `coroio`
targets with a CMake guard, or keep qumir service disabled and link only the
qdb-level `externals/coroio`.

## Backend API

| Endpoint | Method | First-pass behavior |
|---|---|---|
| `/` / static files | `GET` | Serve `service/static/index.html` and assets. |
| `/api/version` | `GET` | Return git hash/branch/date or build metadata. |
| `/api/explain` | `POST` | Accept SQL + browser-provided schema bundle; pipe request to `qdb_plan_export`; return structured plan/runtime bundle. |
| `/api/run` | `POST` | Stub: return `501` or `{ "status": "not_implemented" }`. Button exists but is disabled or shows stub status. |

`/api/explain` request:

```json
{
  "sql": "select ...",
  "dataset": {
    "id": "tpch-browser-mock",
    "tables": [
      {
        "name": "lineitem",
        "stats": {
          "rows": 6001215,
          "bytes": 768155520,
          "rowGroups": 18
        },
        "columns": []
      }
    ]
  },
  "options": {
    "scheduler": "threaded",
    "schedulerWorkers": 18,
    "scanTasks": 18,
    "shufflePartitions": 4,
    "format": "runtime-bundle",
    "embedWasm": true,
    "verboseKernels": true
  }
}
```

`/api/explain` response:

```json
{
  "ok": true,
  "plan": {
    "logicalText": "...",
    "physicalText": "...",
    "runtime": {
      "nodes": [],
      "connections": [],
      "edges": []
    }
  },
  "artifacts": {
    "nodes": {
      "n12": {
        "ast": "...",
        "ir": "...",
        "llvm": "..."
      }
    }
  },
  "diagnostics": []
}
```

The service should treat this request as opaque JSON and forward it to the
console exporter over stdin:

```text
qdb_plan_export --stdin-json --stdout-json
```

The service reads stdout asynchronously with `coroio` and returns it as the HTTP
response. Stderr is captured into the error envelope if the exporter exits with
a non-zero code.

Errors should use the same JSON envelope:

```json
{
  "ok": false,
  "error": {
    "message": "...",
    "stage": "parse|type|lower|compile"
  }
}
```

## Explain Graph Payload

Graph payload should not require parsing text diagnostics. Add structured export
APIs around the existing runtime pieces.

Scheduler graph:

```json
{
  "nodes": [
    {
      "id": "n1",
      "kind": "Source",
      "label": "lineitem",
      "operator": "source",
      "lane": 0,
      "outputType": "struct<...>",
      "artifacts": []
    },
    {
      "id": "n2",
      "kind": "Unary",
      "operator": "filter",
      "label": "Filter",
      "inputType": "struct<...>",
      "outputType": "struct<...>",
      "artifacts": ["filter.ast", "filter.ir", "filter.llvm"]
    }
  ],
  "connections": [
    {
      "id": "c1",
      "kind": "one-to-one",
      "srcCount": 1,
      "dstCount": 1,
      "debugName": "unary-input"
    }
  ],
  "edges": [
    {
      "from": "n1",
      "to": "n2",
      "connection": "c1",
      "srcLane": 0,
      "dstLane": 0
    }
  ]
}
```

## Runtime Bundle JSON With Embedded WASM

`qdb_plan_export` should return a self-contained runtime bundle shape. The web
service only transports this JSON. The bundle is the contract for a future
browser runtime: logical plan text, physical/scheduler graph topology in JSON,
plus WASM kernel blobs embedded as base64.

Top-level shape:

```json
{
  "format": "qdb.runtime.bundle",
  "version": 1,
  "mode": "single-threaded",
  "dataset": {
    "id": "tpch-browser-mock",
    "source": "browser"
  },
  "schemas": {
    "catalog": {},
    "output": {}
  },
  "plans": {
    "logicalText": "...",
    "physicalText": "..."
  },
  "graph": {
    "nodes": [],
    "connections": [],
    "edges": [],
    "result": {
      "connection": "c_final",
      "lane": 0
    }
  },
  "kernels": {},
  "artifacts": {}
}
```

Node format:

```json
{
  "id": "n_filter_1",
  "kind": "unary",
  "operator": "filter",
  "label": "Filter",
  "state": {
    "kind": "filter",
    "inputType": "struct<...>",
    "outputType": "struct<...>"
  },
  "kernels": {
    "filter": "k_filter_1"
  },
  "artifacts": {
    "ast": "a_filter_1_ast",
    "ir": "a_filter_1_ir",
    "llvm": "a_filter_1_llvm"
  }
}
```

Connection format:

```json
{
  "id": "c_unary_input_1",
  "kind": "one-to-one",
  "debugName": "unary-input",
  "srcCount": 1,
  "dstCount": 1,
  "queue": {
    "rowsetCapacityPerLane": 4
  }
}
```

Edge format:

```json
{
  "from": "n_source_1",
  "to": "n_filter_1",
  "connection": "c_unary_input_1",
  "srcLane": 0,
  "dstLane": 0
}
```

Kernel format:

```json
{
  "k_filter_1": {
    "kind": "wasm",
    "module": "m_filter_1",
    "entry": "qdb_filter_dispatch",
    "abi": "qdb.rowset.v1",
    "signature": "void(TRowSet*)",
    "memory": {
      "imported": true,
      "name": "memory"
    }
  }
}
```

WASM module format:

```json
{
  "modules": {
    "m_filter_1": {
      "encoding": "base64",
      "contentType": "application/wasm",
      "bytes": "AGFzbQEAAA..."
    }
  }
}
```

Artifacts are text blobs referenced by node ids:

```json
{
  "artifacts": {
    "a_filter_1_ast": {
      "kind": "ast",
      "encoding": "utf-8",
      "text": "..."
    },
    "a_filter_1_ir": {
      "kind": "ir",
      "encoding": "utf-8",
      "text": "..."
    },
    "a_filter_1_llvm": {
      "kind": "llvm-ir",
      "encoding": "utf-8",
      "text": "..."
    }
  }
}
```

For the single-threaded one-to-one browser runtime subset, the first executable
bundle can restrict:

| Field | First executable subset |
|---|---|
| `mode` | `single-threaded` only |
| connection kinds | `one-to-one` only |
| node kinds | `source`, `unary`, `blocking`, `binary-blocking`, `sink` as needed |
| kernel modules | embedded base64 WASM |
| data source | browser-attached dataset or mock source |

For the first explain-only service, it is acceptable to return `kernels` and
`modules` as empty objects while the graph/artifact format is stabilized. The
important part is that node references already use stable ids (`k_*`, `m_*`,
`a_*`) so adding WASM bytes later does not change graph format.

`logicalText` should match the tree form printed by the current CLI logical plan
printer. `physicalText` should describe the lowered runtime graph: task nodes,
connections, lanes and connection kinds. The graph JSON is authoritative for the
UI; text is for inspection and copy/paste debugging.

Node artifact model:

| Artifact | Meaning | Source |
|---|---|---|
| `ast` | generated Qumir AST/core source for this kernel or logical operator | logical graph export / `TKernelCompiler` diagnostics |
| `ir` | Qumir IR after lowering/optimization | LLVM runner/codegen diagnostics |
| `llvm` | LLVM IR for the compiled kernel | LLVM codegen diagnostics |

Important: current diagnostics are split across two paths. Logical AST can be
exported directly. QumirDB kernel AST is printed to a caller-provided
`std::ostream`, so the first exporter can structure that text. Qumir IR / LLVM
IR is still printed by `TLLVMRunner` to `stderr` when `PrintIr` / `PrintLlvm`
is enabled, so the web exporter should not rely on it yet. The next step is a
structured `TExplainCollector` or similar object passed through
`TKernelCompiler` and `TLLVMRunner`, so AST, Qumir IR and LLVM IR are emitted as
per-kernel artifacts without parsing global streams.

## Browser Dataset Schemas

First implementation keeps schemas in the browser. It is enough for
parse/type/lower and graph explain if source operators can bind against schema
metadata without opening parquet files.

The right pane stores browser-attached datasets:

```json
{
  "id": "tpch-browser-mock",
  "name": "TPC-H Mock",
  "source": {
    "kind": "browser",
    "files": []
  },
  "tables": [
    {
      "name": "lineitem",
      "columns": [
        {"name": "l_orderkey", "type": "i64"},
        {"name": "l_partkey", "type": "i64"},
        {"name": "l_suppkey", "type": "i64"},
        {"name": "l_linenumber", "type": "i64"},
        {"name": "l_quantity", "type": "f64"},
        {"name": "l_extendedprice", "type": "f64"},
        {"name": "l_discount", "type": "f64"},
        {"name": "l_tax", "type": "f64"},
        {"name": "l_returnflag", "type": "string"},
        {"name": "l_linestatus", "type": "string"},
        {"name": "l_shipdate", "type": "date"},
        {"name": "l_commitdate", "type": "date"},
        {"name": "l_receiptdate", "type": "date"},
        {"name": "l_shipinstruct", "type": "string"},
        {"name": "l_shipmode", "type": "string"},
        {"name": "l_comment", "type": "string"}
      ]
    }
  ]
}
```

The initial UI can ship a built-in TPC-H schema-only preset:

```text
region, nation, supplier, customer, part, partsupp, orders, lineitem
```

For real local parquet files during development, use the existing qdb CLI
describe command to obtain actual schemas:

```text
qdb> \d lineitem
```

or from a script by feeding `\d <table>` into qdb. The extracted schema can then
be pasted/imported into the browser dataset pane. Later the browser FS adapter
should inspect attached files directly and construct the same schema bundle.

## Frontend Layout

First screen is the actual tool, not a landing page.

```text
+---------------------------------------------------------------+
| toolbar: [saved] [Run] [Explain] status                       |
+----------+--------------------------------------+-------------+
| left     | SQL editor                           | right       |
| queries  |                                      | datasets    |
| drawer   |                                      | drawer      |
+----------+--------------------------------------+-------------+
| bottom result / explain details table                         |
+---------------------------------------------------------------+
```

UI components:

| Area | First-pass behavior |
|---|---|
| Left pane | Saved queries in `localStorage`; create/rename/delete/select. Slide-out drawer. |
| Center | CodeMirror SQL editor; fallback `<textarea>` if CDN fails. |
| Right pane | Browser-owned dataset list; built-in TPC-H schema preset; schema import/edit; selected schema bundle goes into `/api/explain`. Slide-out drawer. |
| Toolbar | `Run` disabled/stub; `Explain` calls `/api/explain`; small status indicator. |
| Bottom pane | Result table placeholder; for explain, shows selected node/connection details and artifact tabs. |
| Graph view | SVG/canvas/HTML graph; circles for nodes, lines for connections. Click node to show AST/IR/LLVM; hover/click connection to show kind and lane mapping. |

Graph library options:

| Option | Pros | Cons |
|---|---|---|
| Hand-written SVG | No dependency, enough for first graph | Layout algorithm must be simple/layered |
| `dagre` via CDN | Good directed graph layout | Extra dependency |
| `d3` via CDN | Flexible interaction | More code |

First pass should use either simple hand-written layered SVG or `dagre` CDN.
Avoid a heavy app framework until the UI needs it.

## Backend Implementation Steps

- [x] Add qdb-level `externals/coroio` and CMake integration.
- [x] Add `qdb_plan_export` console tool:
  - reads JSON request from stdin;
  - accepts browser-provided schema catalog;
  - builds logical plan;
  - builds physical/scheduler plan;
  - emits runtime bundle JSON to stdout;
  - optionally embeds WASM kernel modules as base64. Not implemented yet.
- [x] Add `service/server.cpp` with a minimal `coroio` router copied in shape
  from qumir, not as shared code.
- [x] Add static file serving with path canonicalization.
- [x] Add `/api/version`.
- [x] Add `/api/explain` as a pipe proxy to `qdb_plan_export`; do not run
  parse/type/lower directly on the HTTP coroutine.
- [x] Add stderr/exit-code handling for exporter failures.
- [x] Add a schema-backed mock `ISource`/catalog path in the exporter for
  explain-only planning from browser-provided schemas.
- [x] Add structured scheduler graph export.
- [ ] Add structured kernel artifact collection for AST/IR/LLVM.
- [ ] Add runtime bundle JSON export with stable node / connection / kernel /
  module / artifact ids.
- [ ] Add optional base64 WASM embedding for compiled kernels.
- [x] Add `/api/run` stub.
- [x] Add CMake target, e.g. `qdb_web`.

## Frontend Implementation Steps

- [x] Add `service/static/index.html`, `styles.css`, `app.js`.
- [x] Implement responsive shell with left/right slide-out panes and bottom pane.
- [x] Add CodeMirror SQL editor via CDN.
- [x] Add saved queries in `localStorage`.
- [x] Add browser dataset model in `localStorage`.
- [x] Add built-in mock TPC-H schema preset.
- [ ] Add schema import/edit UI; first version can paste JSON produced from
  `qdb` `\d <table>` output or hand-written schema JSON.
- [x] Add API helpers for explain/run; dataset operations stay browser-local.
- [x] Add explain graph renderer.
- [x] Add node details panel that expands available artifact refs.
- [x] Add in-graph inspector so selecting a node/connection does not navigate
  away from the graph.
- [x] Add graph mode switch for logical vs physical graph.
- [x] Add connection hover/click details.
- [x] Draw graph edges with direction arrows and connection-kind labels.
- [x] Add table placeholder for future query results.

## Structured Explain Work

Current CLI diagnostics are useful but text-oriented. The structured exporter
should produce:

- `plans.logicalText`: only the logical tree style currently printed by the CLI;
- `plans.logicalAstText`: logical AST for future artifact/detail panes, not for
  the primary logical plan view;
- `graphs.logical`: operator graph with short JSON-friendly node labels
  (`source`, `filter`, `join`, `aggregate`, ...), plus `tooltip` and `details`
  fields for predicates, projections, join keys and residuals;
- `graphs.physical`: physical/scheduler graph with task nodes, connections,
  lane counts and connection kinds.
- `mode`: scheduler mode using the same names as CLI/options (`single` or
  `threaded`).

The browser-owned dataset may include table stats (`rows`, `bytes`,
`rowGroups`). The exporter uses them only for planning/explain: schema-only
sources expose synthetic row groups so threaded lowering can show scan splits,
hash shuffle, gather and merge topology without reading data.

Do not show the physical plan as a large text pane in the first UI. It is too
noisy and hides the topology differences that the graph view is supposed to
make visible.

For the web UI, JSON is authoritative:

- [ ] Add graph export helpers:
  - node id, task kind, operator kind, label;
  - connection id, connection kind, debug name, lane counts;
  - edges with source/destination lane ids;
  - input/output types as strings. Not implemented yet.
- [x] Export separate logical and physical graph payloads.
- [ ] Add kernel artifact collector:
  - [x] associate logical AST artifacts with logical graph node ids;
  - [x] scrape existing runtime-node AST diagnostics into physical graph nodes
    by kernel/task kind;
  - [ ] replace diagnostics scraping with a structured collector owned by the
    compiler/lowerer;
  - [ ] collect generated AST/core source directly from the collector;
  - [ ] collect Qumir IR directly from the LLVM runner/codegen path;
  - [ ] collect LLVM IR directly from the LLVM runner/codegen path;
  - [ ] avoid writing to global streams.
- [ ] Keep CLI `--verbose` behavior unchanged by adapting it to the same
  collector or by keeping the old ostream path in parallel.
- [x] Keep `qdb_plan_export` separate from the HTTP service. The service should
  run it via `coroio` pipe and stream/read the JSON result without blocking the
  event loop.

## Verification

Backend:

- [x] `ninja -C build qdb_web`
- [x] `ninja -C build qdb_plan_export`
- [ ] `curl http://localhost:<port>/api/version`
- [x] `echo '<request-json>' | build/bin/qdb_plan_export --stdin-json --stdout-json`
  returns runtime bundle JSON.
- [ ] `curl -X POST /api/explain` with SQL + browser schema bundle returns graph
  JSON for a simple query.

Frontend:

- [ ] Open service URL.
- [ ] Saved query drawer opens/closes and persists local queries.
- [ ] Dataset drawer opens/closes and shows browser-owned TPC-H schemas.
- [x] `Explain` renders logical and physical graphs for a simple query.
- [x] Clicking a node shows available artifact sections.
- [ ] Clicking a compiled node shows per-kernel Qumir IR and LLVM IR after the
  structured compiler collector is added.
- [ ] Hovering/clicking a connection shows connection kind and lanes.
- [ ] `Run` visibly reports "not implemented" or is disabled.

Regression:

- [ ] `ctest --test-dir build --output-on-failure`

## Open Questions

- Should browser schema import store qdb physical types exactly as printed by
  `\d`, or a logical SQL-ish schema (`decimal`, `char(1)`, `date`) that is
  converted by the exporter? First pass should use whatever the existing planner
  accepts.
- Should `/api/explain` return logical/physical trees as structured JSON in the
  first milestone, or only scheduler graph plus text diagnostics?
- Should graph layout be dependency-free SVG first, or use `dagre` from CDN?
- How much of kernel diagnostics should be collected per node before we start
  the service UI? Minimal first pass can show graph first, artifact tabs empty
  until structured collector lands.
