# Qumir boolean short-circuit mis-types a local bool

Generated filter kernels fail during AST-to-IR lowering when `&&` combines a
boolean literal or expression with a local `bool` produced by nullable column
materialization.

Minimal shape:

```oz
(block
  (fun test ((var bitmap <ptr u8>) (var row i64)) -> bool
    (block
      (var valid bool)
      (= valid (call bitoff bitmap row (: 0 i64)))
      (return (&& #t valid)))))
```

Observed error:

```text
Cannot unify types of different kinds: 0 and 13 ids: 4 and 8
```

IR kinds `0` and `13` are `I1` and `Ptr`. The local is declared and assigned
as `bool`, but short-circuit lowering appears to use its address/type instead
of its loaded value on one path.

Qdb currently avoids this by materializing SQL three-valued boolean logic with
numeric states rather than composing generated bool locals.

A bool-valued `if` is not a viable workaround either. The following shape:

```oz
(if valid value #f)
```

can reach LLVM as an invalid mixed-type select:

```llvm
select i1 %valid, i1 %value, i64 0
```

LLVM verification then fails with `Invalid operands for select instruction`.
