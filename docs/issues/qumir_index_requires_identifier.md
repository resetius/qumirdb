# Qumir: pointer indexing lowering requires an identifier

Status: Open.

## Reproducer

An annotated `TIndexExpr` accepts any expression whose type is a pointer, but
lowering rejects a computed pointer collection such as an explicit cast:

```text
(index (cast raw <ptr i32>) row)
```

The error is `COLLECTION_NOT_ARRAY`. Assigning the cast to a temporary and
indexing the identifier works:

```text
(var values <ptr i32>)
(= values (cast raw <ptr i32>))
(index values row)
```

## Cause

`TAstLowerer::LowerLValueAddress` and the `TIndexExpr` lowering path require
`index->Collection` to be `TIdentExpr`, even after type annotation has accepted
the collection as a pointer.

## QDB workaround

Generated column materialization emits typed temporary pointer variables before
all pointer indexing. No Qumir behavior is changed.
