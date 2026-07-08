# Kernel compilation phases

Status: Implemented.

Kernel handling is split into two phases; adding an operator only requires
teaching generation to emit its kernels, and JIT (CLI) vs wasm (JSON/browser)
is the only fork.

```
logical plan
  │  physical compilation (AST level)
  ▼
TLoweredPlan  =  TTaskGraph + Kernels: every kernel as TGeneratedKernel
                 { Name, Stage, Entrypoints, Ast, Storage, Operator, Slot,
                   SortKeys }  (AST only, not compiled)
  │  machine finalization (the ONLY fork)
  ├── JitFinalizeKernels  → compile each AST with the native JIT, fill the
  │                          kernel's TKernelSlot; runtime dispatches read the
  │                          slot (CLI: cli.cpp / RunPlanIntoSink)
  └── WasmFinalizeKernels → compile each AST to a wasm64 object, attach as
                             base64 artifacts (bin/plan_export.cpp)
```

## Generation (qdb/kernel/compiler.{h,cpp})

Every operator's kernels go through `TKernelCompiler::EmitKernel`: build the
program AST, create an empty `TKernelSlot`, append the `TGeneratedKernel` to
`TKernelCompilerOptions::Sink`, and — only when `BindNow` (the default, used by
direct callers such as unit tests) — JIT immediately. The returned dispatches
are closures over the slot (`slot->Fns[entryIndex]`), so filling one slot
late-binds every copy of the dispatch, including those already captured in
per-partition scheduler states.

Kernels per operator:
- filter / project: one kernel, one entry (`<kernel>` / `<project>`), with
  `Storage` holding the literal strings the AST points into.
- sort: `sort.radix.fused` + `sort.radix.nullable.fused`, with resolved
  `SortKeys` `{Index, WidthBytes, Desc}` metadata for the exec exporter.
- aggregate: three kernels (`aggregate.update`/`.measure`/`.finalize`), one
  entry each (the three programs are built independently).
- join: one multi-entry kernel (jt_init, jt_process_*, jt_probe_*_stream,
  jt_destroy, pb_destroy, plus semi/anti/outer finalizers), resolved via
  LookupMany; `join_hash` (jt_hash_left/right) for the partitioned path.

Lowering (`plan_lowerer.cpp`) creates compilers via `KernelOptions(stage, op)`:
`Sink = &TLoweredPlan::Kernels`, `BindNow = false`, `Operator` = the logical
operator, `Stage` = the node's DebugGroup. No target is mentioned anywhere in
this phase.

## Finalization

- JIT: `JitFinalizeKernels` (qdb/kernel/finalize.cpp) — per kernel, a fresh
  `TLLVMRunner` from `KernelRunnerOptions()` + `NativeCode`, `CompileKernelAst`
  (LookupMany), fill `Slot->Fns` in `Entrypoints` order, store the runner in
  the slot (JIT'd pointers live as long as the runner). Idempotent.
  `RunPlanIntoSink` always finalizes before scheduling; cli.cpp finalizes
  earlier so the reported query time excludes kernel compilation.
- Wasm: `WasmFinalizeKernels` (bin/plan_export.cpp) — per kernel, an `ast`
  artifact always and (embed mode) `CompileKernelAstToWasm` = the same runner
  pipeline with `TargetTriple=wasm64-unknown-unknown` via
  `CompileKernelAstToObject`, linked with `wasm-ld -mwasm64`. Slots stay
  unbound; the exporter never executes the graph.

Both finalizers share the frontend (`EnsureQumirDbUse` + the runner's
`EmitKernelArtifacts`); only emission differs. See
docs/issues/browser-wasm64-layout.md for why the browser target is wasm64.

## Consumers

- Physical graph JSON: kernel artifacts attach to graph nodes by
  `Stage == TTaskNode::DebugGroup`.
- The exec (browser) section reads kernels by `Operator` identity
  (`BuildKernelIndex`/`FindKernel` in plan_export.cpp) — stage wasm ids point
  at the same artifacts the physical graph references; nothing is compiled
  twice. Sort stages take `radixKeys` from the kernel's `SortKeys` metadata,
  falling back to JS-comparison keys for non-radix (string) or nullable keys.
