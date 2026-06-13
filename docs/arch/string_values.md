# String values in kernels

Status: proposed architecture. The implementation plan is tracked in
`PLAN_AGGREGATION.md`, section M13.

## Current mismatch

QDB exposes Arrow strings as a variable-width `TColumn`:

```text
Data        -> contiguous UTF-8 bytes
Offsets     -> i32[] or i64[]
OffsetWidth -> 4 or 8
Mask        -> optional validity bitmap
```

Qumir `string` is a different, managed runtime object. It is represented by a
pointer to NUL-terminated data with a refcounted header. Casting `TColumn.Data`
to `<ptr string>` is therefore invalid. The current filter generator does
exactly this for every logical field type, so string predicates are not a
working path today.

Aggregation has an additional lifetime problem: input batches are released
immediately after update, while group keys must remain valid until finalize.

## Proposed value types

QDB kernels must not use Qumir `TStringType` or the Qumir managed `string`
runtime at all. `TStringType` may remain the logical schema marker received
from Arrow, but kernel lowering replaces it with QDB-owned named struct types.

Use two distinct named POD structs with the same physical shape:

```text
StringView  = { Data: <ptr u8>, Size: i64 }
OwnedString = { Data: <ptr u8>, Size: i64 }
```

`StringView` is borrowed from a `TRowSet` and is valid only while that rowset is
alive. `OwnedString` points to immutable bytes owned by the aggregation table
or, later, by the query allocator.

These are ordinary structs in the Qumir AST. They must not trigger string
retain/release, NUL-terminated operations, Unicode indexing, or any other
managed-string lowering.

`OwnedString` is a semantic name, not per-value RAII. Copying or swapping the
two-field struct does not transfer or duplicate ownership. This property is
required because Robin Hood insertion moves keys and the same logical key is
also referenced by dense output storage.

Lengths are byte lengths, not Unicode symbol counts. Hashing, equality,
ordering, Parquet offsets, and CSV output all operate on UTF-8 bytes.

## Column materialization

One generated helper must materialize logical row values for both filter and
aggregation. For a string column and row `i` it reads `Offsets[i]` and
`Offsets[i + 1]`, handling both offset widths, and constructs a `StringView`.

The Parquet source currently normalizes `Data` to the first selected value but
leaves offsets based at `Offsets[0]`. Materialization must therefore use:

```text
start = offsets[i]     - offsets[0]
end   = offsets[i + 1] - offsets[0]
view  = { Data + start, end - start }
```

This logic must not be duplicated between filter and aggregation.

## Why lookup and storage keys differ

The current hash table uses one generic `Key` type for both input lookup and
stored entries. That would force a string copy before every lookup, including
duplicates. It would also require rollback/free behavior for a temporary owned
key.

Instead, generated aggregation code should derive two representations:

```text
LookupKey  = fixed fields + StringView leaves
StoredKey  = fixed fields + OwnedString leaves
```

For a scalar string these are the corresponding scalar named types. For a
composite key, QDB generates parallel named structs with identical logical
field order but different string leaf types.

The table API then becomes conceptually:

```text
lookup(StoredKey[], LookupKey) -> dense slot or -1
insert(StoredKey[], StoredKey, dense slot)
rehash(StoredKey[] -> StoredKey[])
```

The input remains borrowed during lookup. Only an absent key is cloned into
owned storage.

## Generated type helpers

The AST generator must recursively produce helpers for the concrete pair of
types:

```text
rh_hash(LookupKey) -> i64
rh_hash(StoredKey) -> i64
rh_key_equal(StoredKey, LookupKey) -> bool
rh_key_equal(StoredKey, StoredKey) -> bool
key_owned_bytes(LookupKey) -> i64
key_clone_owned(LookupKey, <ptr u8>) -> StoredKey
```

Both hash overloads must use the same byte hash and struct combiner. Equality
must compare lengths first and then bytes. Composite helpers recurse through
nested fields and ignore generated padding.

The first implementation should use a small, stable Oz byte hash with fixed
test vectors. A faster hash can replace it later only with deliberate
compatibility and performance testing. Hash values are process-local table
details; they are not persisted.

## Owned byte storage

The first correct implementation should allocate one contiguous byte block per
new group, containing all string leaves of its key. Generated clone code sets
each `OwnedString.Data` into that block.

The table owns a registry of these blocks and frees them in `aht_destroy`.
Rehash copies `OwnedString` handles and never clones or frees their bytes.

This is intentionally simpler than designing the final query allocator now.
It costs one allocation per unique string-bearing group. Later, the registry
can be replaced by a chunked query arena while keeping `OwnedString` and the
generic table contract unchanged.

Allocation must happen only after borrowed lookup reports a miss. A failure
during clone/registration must leave the table unchanged and return an error.

## String output

The current fixed-width finalize ABI allocates `row_count * field.Size`, which
cannot represent strings. String key output needs Arrow-compatible column
storage:

```text
Data        -> copied UTF-8 bytes
Offsets     -> i64[row_count + 1]
OffsetWidth -> 8
```

Use a two-pass finalize:

1. Measure total output bytes for every string group-key column.
2. C++ allocates output `Data` and `Offsets` buffers.
3. Generated finalize writes offsets and copies bytes from dense `StoredKey`
   values.
4. The table may then be destroyed; output no longer references its arena.

`TAggregateOutputKey` must become a representation descriptor rather than only
`Size` and `Alignment`. The generated finalize ABI should receive output
`TColumn` descriptors, or an equivalent structure carrying both data and
offset buffers.

## Filtering

Filtering should consume the same generated `StringView` values, but it does
not need owned strings. The filter expression boundary needs explicit
StringView operations for at least:

- equality and inequality;
- lexical ordering;
- optional prefix/contains helpers later.

Column-to-column comparisons can be fully `StringView` based. String literals
in query expressions must also be lowered by QDB into static byte storage plus
a `StringView` struct. They must not first become Qumir managed strings.

Parsing and annotation of the final kernel AST must see both column bindings
and literals as `StringView`. Otherwise Qumir can select managed-string
operators and insert retain/release behavior inappropriate for Arrow memory.

The current filter path annotates the logical predicate through a wrapper whose
string parameters are `TStringType`. String support therefore requires a
separate kernel specialization pass: clone/rewrite the predicate's string
field bindings and literals to `StringView`, then resolve and annotate the
complete generated kernel. Reusing an already annotated managed-string
predicate is not sufficient.

## Null semantics

Null representation is not string-specific. Every `TColumn` uses the same
`Mask + MaskBitOffset` validity bitmap, regardless of its value type.
`StringView` describes only the bytes of a valid string value.

The column format determines whether a row value is null, but operator code
still determines what null means:

- `GROUP BY`: null keys participate in grouping; nulls in the same key position
  compare equal for grouping, and their payload bytes must not be read;
- `WHERE`: a null/unknown predicate does not select the row; correct compound
  nullable predicates eventually require three-valued logic;
- `count(*)` counts rows, `count(arg)` skips null arguments, and `sum/min/max`
  skip null inputs and need a defined empty-group result;
- output columns carry nulls through their own `TColumn.Mask`.

Validity should therefore be added once to the common column materializer and
generated key/reducer semantics, for fixed-width and string values together.
It must not be encoded inside `StringView` or implemented as a separate string
rule.

## Required invariants

- A `StringView` never outlives its input `TRowSet`.
- A null value is represented by column validity, not by a special
  `StringView{Data,Size}` value.
- Stored keys never reference input batch memory.
- `OwnedString` bytes remain immutable until table destruction.
- Equal lookup/stored keys always produce the same hash.
- Robin Hood displacement and rehash never allocate or free string bytes.
- Final output owns its bytes and does not reference the table arena.
- String comparisons use byte length and do not depend on NUL termination.
- No generated QDB kernel value has Qumir `string` type.
