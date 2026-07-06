# WASM Browser Runtime Plan

Goal: move qdb pipeline execution to a browser-capable runtime where orchestration can be implemented in C++ or JS, while complex runtime structures are owned by qumir-generated kernels and exposed only as opaque handles.

Key assumption: a service generates a JSON plan and WASM kernels. The browser runtime loads the plan, instantiates kernels, schedules operators, and passes opaque handles between calls.

## Core Direction

Complex structures should live inside qumir/WASM runtime code:

- hash tables;
- aggregation states;
- join row stores;
- string arenas;
- selection vectors;
- temporary buffers;
- operator scratch arenas.

C++/JS orchestration should store and pass only:

- `operator_handle`;
- `state_handle`;
- `batch_handle`;
- JSON metadata;
- dependency graph state.

In browser/WASM, an opaque pointer is not a native pointer. It is an offset or handle inside WASM linear memory, typically `u32` or `u64`. JS must not dereference it; JS only passes it back into exported WASM functions.

## Checklist

- [ ] **1. Define ownership model**

  Kernel/runtime side owns all complex data structures. Orchestration owns only scheduling metadata and opaque handles.

  This should cover:

  - operator states;
  - batches;
  - hash tables;
  - arenas;
  - string storage;
  - temporary vectors;
  - finalize output buffers.

- [ ] **2. Define stable kernel ABI**

  Minimal generic operator ABI:

  ```c
  qdb_status operator_init(plan_ptr, plan_len, out_state_handle);
  qdb_status operator_process(state_handle, input_batch_handle, out_batch_handle);
  qdb_status operator_finalize(state_handle, out_batch_handle);
  qdb_status operator_free(state_handle);
  qdb_status batch_free(batch_handle);
  ```

  Join should use split-input entrypoints:

  ```c
  qdb_status join_init(plan_ptr, plan_len, out_state_handle);
  qdb_status join_process_left(state_handle, batch_handle);
  qdb_status join_process_right(state_handle, batch_handle);
  qdb_status join_finalize(state_handle, out_batch_handle);
  qdb_status join_free(state_handle);
  ```

- [ ] **3. Define opaque batch contract**

  Preferred MVP: `batch_handle` is opaque. The orchestrator sees schema metadata from JSON, but not internal batch layout.

  Later options:

  - expose a small batch descriptor for result decoding;
  - support Arrow-like buffers at boundaries;
  - keep internal execution batches fully opaque.

- [ ] **4. Move stateful structures into qumir kernels**

  Qumir-generated code needs support for:

  - `init`;
  - `process`;
  - `finalize`;
  - `free`;
  - arena allocation;
  - state structs;
  - nested runtime structs;
  - deterministic cleanup on error.

  This is the main prerequisite before browser work.

- [ ] **5. Prove the ABI in native C++ first**

  Before JS/browser, add a C++ orchestration path that uses the same opaque ABI:

  - no direct access to join/agg state layout;
  - all stateful operators called through handles;
  - old native executor remains as reference;
  - compare results and timings on TPC-H.

- [ ] **6. Define JSON plan contract**

  JSON should include:

  - operator ids;
  - operator kinds;
  - dependency edges;
  - input/output schemas;
  - kernel module URLs or ids;
  - export names;
  - constants;
  - join side mapping;
  - memory limits;
  - expected batch sizes.

  Example shape:

  ```json
  {
    "operators": [
      {
        "id": 7,
        "kind": "hash_join",
        "wasm": "kernels/q9_join_2.wasm",
        "exports": {
          "init": "qdb_join_init",
          "left": "qdb_join_left",
          "right": "qdb_join_right",
          "finalize": "qdb_join_finalize",
          "free": "qdb_join_free"
        }
      }
    ]
  }
  ```

- [ ] **7. Build JS scheduler MVP**

  Initial version:

  - single-threaded;
  - load JSON plan;
  - load/instantiate WASM modules;
  - initialize operators;
  - move opaque batches through DAG;
  - call `process` and `finalize`;
  - free all handles.

  Later version:

  - Web Worker scheduler;
  - backpressure;
  - cancellation;
  - per-operator counters;
  - streaming fetch sources.

- [ ] **8. Decide source layer**

  MVP should avoid parquet in browser:

  - server sends pre-columnar binary chunks;
  - source kernels wrap chunks into opaque batch handles;
  - parquet decoding stays server-side.

  Later:

  - Arrow IPC;
  - parquet-wasm;
  - browser cache;
  - streaming object storage reads.

- [ ] **9. Decide memory model**

  Start with one shared WASM runtime memory for the whole pipeline.

  Rationale:

  - opaque handles can move between operators without serialization;
  - batches do not need cross-memory copying;
  - allocator and arenas are centralized.

  Module-per-operator with separate memories should be avoided initially because opaque handles would not be portable across modules.

- [ ] **10. Add numeric MVP operators**

  Implement first:

  - numeric source batches;
  - numeric filter;
  - numeric project;
  - numeric hash aggregate;
  - numeric inner hash join.

  Defer:

  - string keys;
  - residual joins;
  - semi/anti joins;
  - outer joins;
  - sort/top-N;
  - parquet.

- [ ] **11. Add string support**

  String data should remain inside WASM-owned memory:

  - string views are WASM-side structs;
  - bytes live in WASM arenas;
  - JS does not compare strings;
  - JS decodes only final output when needed.

- [ ] **12. Add full join/aggregate support**

  Extend stateful operators:

  - string hash aggregate keys;
  - string join keys;
  - residual filters;
  - semi joins;
  - anti joins;
  - outer joins;
  - adaptive streaming join behavior.

- [ ] **13. Validate browser constraints**

  Check early:

  - WASM SIMD support;
  - memory growth limits;
  - large allocation behavior;
  - SharedArrayBuffer requirements;
  - COOP/COEP headers for threads;
  - worker transfer overhead;
  - final result decoding cost.

- [ ] **14. Add observability**

  Runtime should report:

  - per-operator input/output rows;
  - process/finalize time;
  - allocated bytes;
  - peak memory;
  - batch counts;
  - kernel errors;
  - cancellation point.

## Milestones

- [ ] Native opaque ABI prototype for one numeric aggregate.
- [ ] Native opaque ABI prototype for one numeric join.
- [ ] JSON plan schema for a single-pipeline query.
- [ ] JS scheduler runs numeric filter/project/aggregate from WASM.
- [ ] JS scheduler runs numeric hash join from WASM.
- [ ] Browser demo with server-provided columnar chunks.
- [ ] String batch support.
- [ ] TPC-H subset in browser runtime.

## Main Risk

The hard part is not calling WASM. The hard part is defining a stable ABI and ownership model that keeps stateful execution inside qumir-generated runtime code while making C++ and JS orchestration thin, equivalent, and debuggable.

The desired final shape:

```text
read JSON plan
load kernels
init opaque states
move opaque batches through DAG
call process/finalize
free handles
```

All layout-heavy work stays in qumir/WASM runtime: hash tables, arenas, string views, join stores, aggregate states, and temporary execution buffers.
