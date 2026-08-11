# SQL expression pseudo-calls

QDB uses call-shaped AST nodes for SQL semantics that do not map one-to-one to
a runtime function. They preserve information through parsing and logical plan
rewrites, then `qdb/kernel/annotate_type.cpp` expands them once operand types and
nullability are known.

Keep this list updated when adding another call name handled specially by
`InferType`, `ExpandNullable`, or kernel generation.

## Expansion-only calls

| Call | Produced by | Expansion |
|---|---|---|
| `qdb_in_list(lhs, item...)` | SQL scalar `IN (...)` | Binds `lhs` once, hoists its nullable guard, and builds equality/OR expressions over its plain value. Per-item three-valued logic remains only for nullable items. The call never reaches Qumir name resolution. |
| `strcat(a, b)` | SQL `a || b` | Becomes `qdb_string_concat(__arena__, a, b)`. `__arena__` is the project kernel's hidden scratch-arena parameter. `strcat` itself is not a runtime symbol. |
| `regexp_replace(str, pattern, replacement)` | SQL function syntax | Constant `pattern` and `replacement` are registered once per query, then the call becomes `qdb_regexp_replace(__arena__, __regexes__[id], str)`. The current three-argument form uses the ECMAScript regex dialect and replaces only the first match; flags are not supported yet. |
| `coalesce(a, ...)` | SQL function syntax | Becomes a typed `if` chain selecting the first valid argument. |

`qdb_in_list` unwraps `Nullable[T]` internally only after testing `Valid`. There
is intentionally no standalone `qdb_unwrap_nullable` call: an unguarded unwrap
would make it easy to discard SQL NULL semantics.

## Semantic bridge calls

These names have implementations or overloads in `qdb/modules/qumirdb.oz`, but
QDB typing also recognizes them specially because their SQL null behavior is
not ordinary null propagation.

| Call | Contract |
|---|---|
| `qdb_sql_null()` | Represents a bare, initially untyped SQL `NULL`. Its enclosing expression supplies the eventual `Nullable[T]` value type. |
| `qdb_is_null(x)` | SQL `IS NULL`: `!x.Valid` for `Nullable[T]`, and constant `false` for non-nullable `T`. |
| `qdb_is_true(x)` | Predicate boundary used by `WHERE`, `HAVING`, join residuals, and `CASE WHEN`. It accepts a row only when the SQL value is true; `NULL` is not true. |

## Kernel placeholders

`__arena__` and `__regexes__` are identifier placeholders rather than calls.
Filter and project kernel generation declares them as the opaque per-invocation
string scratch arena and the query-local compiled-regex handle table. Computed
sort, grouping, aggregate, and window expressions are materialized by a project
before their key kernels. A regex call left directly in a join or low-level
aggregate kernel is rejected explicitly instead of reaching the linker.
