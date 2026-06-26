# Aggregation kernels

This directory contains standalone oz-lang sources used to develop and test
the aggregation hash table before it is connected to the physical pipeline.

Production aggregation specializes the same Robin Hood algorithm for scalar
and composite fixed-width integer/`f64` keys. The algorithm uses a template key
type:

```text
<named Key (template)>
```

Operations that depend on the key representation (`rh_hash` and equality) are
generated concrete overloads. Adding another primitive leaf type must not
require changing the probing algorithm.

Sources that use QumirDb external types or functions are run by
`test_aggregation`, which registers `QumirDbModule` before compiling them.
Pure core-language examples may also be run with `qumiri` or compiled with
`qumirc`.

Production and current generic tests use `CompileKernelAst(ast, entryName, ...)`
with explicit entry names. Old standalone examples may still depend on source
order and are retained only as regression fixtures.

Historical pair-key experiments are intentionally split across separate sources:
`generic_dispatch_i64.oz`, `generic_dispatch_pair_i64.oz`,
`generic_pair_fixed.oz`, and `generic_pair_rehash.oz`. Files whose wrapper calls
a generic function give the wrapper the same ABI as the specialization that the
current runner returns.
