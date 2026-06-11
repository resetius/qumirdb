# Aggregation kernels

This directory contains standalone oz-lang sources used to develop and test
the aggregation hash table before it is connected to the physical pipeline.

Stage 1 supports only `i64` keys. The Robin Hood algorithm should still use a
template key type:

```text
<named Key (template readable mutable)>
```

Operations that depend on the key representation (`rh_hash`, equality and
copying) are overloads. Initially only their `i64` overloads are implemented.
Adding another key type must not require changing the probing algorithm.

Sources that use QumirDb external types or functions are run by
`test_aggregation`, which registers `QumirDbModule` before compiling them.
Pure core-language examples may also be run with `qumiri` or compiled with
`qumirc`.

`TLLVMRunner::CompileKernel` currently returns the last lowered function. Keep
the concrete entry-point function last. A wrapper that calls generic functions
may instantiate specializations after itself, so callable test entry points
must remain concrete until the runner exposes explicit entry-point selection.

Stage J pair-key experiments are intentionally split across separate sources:
`generic_dispatch_i64.oz`, `generic_dispatch_pair_i64.oz`,
`generic_pair_fixed.oz`, and `generic_pair_rehash.oz`. Files whose wrapper calls
a generic function give the wrapper the same ABI as the specialization that the
current runner returns.
