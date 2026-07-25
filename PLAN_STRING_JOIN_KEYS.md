# String Join Keys — пошаговый план

## Контекст

String join keys сейчас не работают не потому, что descriptor их не понимает.
`BuildJoinKeyDescriptor` уже переиспользует `RepresentKeyType(...)` и строит
две репрезентации:

- `LookupType`: borrowed `StringView`;
- `StoredType`: owned `OwnedString`;
- для string-bearing composite key — параллельные named structs
  `JoinKey_..._Lookup` / `JoinKey_..._Stored`.

Но join generator всё ещё содержит production guard:

```cpp
if (key.HasDistinctLookupType()) {
    throw TError("... string keys are not supported yet ...");
}
```

То есть проблема не в schema/type descriptor, а в том, что generic join table
path пока рассчитан на один тип `Key`, где lookup key и stored key совпадают.
Для string keys нужно сделать join таким же dual-key, как string aggregation:
probe borrowed key against stored owned keys, а на insert miss clone key bytes
into table-owned storage.

---

## Цель

Поддержать string и string-bearing composite equi-join keys для:

- [ ] `Inner`
- [ ] `LeftSemi`
- [ ] `LeftAnti`
- [ ] `Left` / `Right` outer, если базовый dual-key path не усложняет finalize

Сохранить текущую производительность fixed-width joins: если
`LookupType == StoredType`, текущий fast path должен остаться без лишней
string ownership логики.

---

## Основной дизайн

### Fixed-width path

Остаётся текущий:

```text
LookupKey == StoredKey == Key
jt_emit_and_insert(HashTable, HashTable, Key, row_id, ...)
```

### String-bearing path

Новый dual-key path:

```text
LookupKey = StringView/composite with StringView leaves
StoredKey = OwnedString/composite with OwnedString leaves
```

Probe:

```text
rh_lookup_dual(stored_keys, dist, slot_id, capacity, lookup_key)
```

Insert:

```text
owned_bytes = key_owned_bytes(lookup_key)
owned_block = qdb_alloc(owned_bytes)
stored_key = key_clone_owned(lookup_key, owned_block)
rh_insert_stored(..., stored_key, dense_slot)
aht_owned_blocks_commit(table, owned_block)
```

Rehash moves `StoredKey` handles only; it never copies/free string bytes.

Hash/equality overloads already exist from `GenJoinKeyOpsFunDecls`, because it
wraps `GenKeyOperationFunDecls`:

- `rh_hash(LookupKey)`
- `rh_hash(StoredKey)`
- `rh_key_equal(StoredKey, LookupKey)`
- `rh_key_equal(StoredKey, StoredKey)`

Ownership helpers also already exist:

- `key_owned_bytes(LookupKey) -> i64`
- `key_clone_owned(LookupKey, <ptr u8>) -> StoredKey`

---

## Шаги реализации

### Шаг 1 — подтвердить и покрыть descriptor

Файлы:

- `test/test_join_key.cpp`
- `test/test_string_key_descriptor.cpp`

Работа:

- [ ] Убедиться, что `BuildJoinKeyDescriptor` уже строит distinct
      lookup/stored type для scalar string key.
- [ ] Убедиться, что composite `{i64, string}` join key строит matching
      layout/padding как aggregation.
- [ ] Добавить недостающие tests, если они покрывают только aggregation.

Ожидание: этот шаг, скорее всего, почти готов.

### Шаг 2 — добавить dual-key join Oz library

Новый файл:

- `qdb/kernel/join/join_update_dual_key.oz`

Переиспользовать идеи из:

- `qdb/kernel/aggregation/robin_hood_dual_key.oz`
- `qdb/kernel/aggregation/owned_blocks.oz`

Нужные функции:

- [ ] `jt_lookup_dual(build, lookup_key) -> dense_slot`
- [ ] `jt_insert_lookup_owned(own, lookup_key, own_row_id) -> bool`
- [ ] `jt_rehash_dual(own, new_capacity, stored_witness) -> bool`
- [ ] `jt_probe_and_emit_dual(build, lookup_key, row_id, is_left, pairs, stores, stream_batches)`
- [ ] `jt_emit_and_insert_dual(own, build, lookup_key, row_id, ...)`
- [ ] `jt_insert_slot_only_dual(own, lookup_key) -> bool`

Important: join table uses dense row buckets, not aggregate reducers.
Initialization for a new dense slot must still set:

```text
AggBuffers[0][slot] = 0   ; row count
AggBuffers[1][slot] = 0   ; row bucket capacity
AggBuffers[2][slot] = 0   ; row bucket data pointer
```

Unlike aggregation, there is no reducer update; after dense slot creation,
append `own_row_id` with `jb_append`.

### Шаг 3 — reuse owned block registry

Current string aggregation stores owned key byte blocks in `HashTable` fields
managed by `owned_blocks.oz`. Join already reuses `HashTable`, so string join
must use the same ownership registry.

Work:

- [ ] Ensure `BuildJoinKernelLibrary()` loads `owned_blocks.oz` before the
      dual-key join update code when string keys are used.
- [ ] Ensure `jt_destroy` frees owned blocks for join hash tables.
- [ ] Verify fixed-width join destroy remains unchanged.

Potential implementation choice:

- always include `owned_blocks.oz` in join kernel library; or
- include it only in dual-key program construction.

Prefer the simpler first version if it does not change fixed-width behavior
after optimization.

### Шаг 4 — generate lookup keys from batches

Files:

- `qdb/kernel/join_gen.cpp`
- `qdb/kernel/join_gen.h`

Current `GenJoinBatchAst` builds `key.KeyType` and calls fixed-key functions.
For distinct lookup/stored joins it must build `key.LookupType`.

Work:

- [ ] Remove `HasDistinctLookupType()` throw from `GenJoinProcessAst`.
- [ ] Remove the same throw from `GenJoinProbeAst`.
- [ ] Remove or replace the throw from `GenJoinInsertKeyOnlyAst`.
- [ ] Keep `GenJoinFinalizeSemiAntiAst` fixed/stored-key oriented, because it
      scans stored `GroupKeys`; if needed, generate a dual finalize variant.
- [ ] Pass `stringViewType` into join key materialization so
      `BuildColumnValueAst` can produce `StringView`.
- [ ] For each key field, materialize from the left/right batch into borrowed
      lookup representation.
- [ ] Construct `LookupType` key struct:
      - `valid_N` fields for nullable keys;
      - `key_N` fields as materialized lookup values;
      - generated padding fields as zero.

The resulting lookup key is valid only for the current batch row.

### Шаг 5 — choose fixed vs dual generated entry wrappers

Files:

- `qdb/kernel/compiler.cpp`
- `qdb/kernel/join_gen.cpp`

Work:

- [ ] If `!keyDesc.HasDistinctLookupType()`, generate current fixed-key wrappers.
- [ ] If `keyDesc.HasDistinctLookupType()`, generate dual-key wrappers that call
      `jt_emit_and_insert_dual` / `jt_probe_and_emit_dual`.
- [ ] Compile the same C++ ABI:
      - `ProcessLeft`
      - `ProcessRight`
      - `ProbeLeftStream`
      - `ProbeRightStream`
      - `InsertKeyOnly`
      - `FinalizeAntiSemi`
      - `FinalizeOuter`
- [ ] Keep runtime code unaware of key representation.

The runtime should not branch on string keys; only compiled kernels differ.

### Шаг 6 — finalize paths for semi/anti and outer

`FinalizeAntiSemi` and `FinalizeOuter` scan stored group keys from one table and
probe the opposite table.

For dual-key joins, both sides' `GroupKeys` are `StoredKey`. Need stored-vs-
stored lookup:

```text
rh_lookup_stored(stored_keys, ..., stored_key)
```

This can use `rh_key_equal(StoredKey, StoredKey)` and `rh_hash(StoredKey)`.

Work:

- [ ] Generate or add Oz helper for stored-key lookup.
- [ ] Update dual finalize to scan `own.GroupKeys` as `StoredKey`.
- [ ] Probe `opp` using stored key.
- [ ] Emit row buckets exactly like current finalize.
- [ ] Cover:
      - `LeftSemi` string key;
      - `LeftAnti` string key;
      - `Left` string key;
      - `Right` string key.

### Шаг 7 — residual compatibility

Residual predicates are orthogonal to key representation. They read payload
columns from stores or transient stream batches by row-id.

Work:

- [ ] Ensure dual-key `jt_probe_and_emit_dual` calls the same
      `jt_residual_filter(...)` before `pb_push`.
- [ ] Verify `Inner + residual + string key`.
- [ ] Verify `LeftSemi/LeftAnti + residual + string key`, especially current
      right-side streaming path.

### Шаг 8 — tests

Kernel-level tests:

- [ ] `CompileJoin` accepts scalar string key.
- [ ] `ProcessLeft/ProcessRight` with string keys matches nested-loop.
- [ ] `ProbeRightStream` with string key emits matches and does not insert
      stream rows.
- [ ] composite key `(i64, string)` works.
- [ ] nullable string key obeys existing null key semantics.
- [ ] rehash with many distinct string keys preserves row buckets and owned
      strings.
- [ ] destroy frees owned string blocks without leaks/crashes.

Runtime tests:

- [ ] `TRuntimeJoin Inner` with string key, multiple batches.
- [ ] adaptive streaming after EOF with string key.
- [ ] `LeftSemi` / `LeftAnti` with string key.
- [ ] `Left` / `Right` outer with string key.
- [ ] residual string-key join.

Planner/e2e tests:

- [ ] sexp query joining two sources on string columns.
- [ ] query with composite string key.
- [ ] TPC-H query coverage if any existing query uses string equi-key after
      rewriting.

### Шаг 9 — validation / performance

Commands:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
cd build && ../benchmark/tpch/run_tpch.sh ~/Projects/tpch/ 1
```

Additional focused runs:

```bash
./build/test/test_join_key
./build/test/test_string_key_descriptor
./build/test/test_join_kernel
./build/test/test_join_exec
./build/test/test_join_planner
```

Performance expectations:

- fixed-width joins should be unchanged within noise;
- string joins will pay clone cost only on insert miss;
- probe-only streaming must not clone stream-side string keys, because streamed
  rows are never inserted.

---

## Implementation order

1. [ ] Add/confirm descriptor tests for join string key lookup/stored types.
2. [ ] Add dual-key join Oz helpers and include ownership helpers.
3. [ ] Generate lookup-key wrappers for process/probe/insert-key-only.
4. [ ] Compile fixed vs dual wrappers based on `HasDistinctLookupType()`.
5. [ ] Add kernel tests for inner string joins.
6. [ ] Add stored-key finalize for semi/anti and outer.
7. [ ] Add runtime tests for inner/semi/anti/outer string joins.
8. [ ] Add residual string-key tests.
9. [ ] Run full ctest and TPC-H.

---

## Risks / details to watch

- `HashTable.KeySize` must be `StoredType` size, not `LookupType` size.
- `HashTable.Keys` and `GroupKeys` must always store `StoredKey`.
- Lookup keys are borrowed from input `TRowSet`; never store a `StringView`.
- On allocation failure after `qdb_alloc`, rollback must free the owned block
  and leave table size/probe arrays unchanged.
- Rehash must move `OwnedString` handles only, not clone or free string bytes.
- `FinalizeAntiSemi` / `FinalizeOuter` must probe stored-vs-stored, not
  lookup-vs-stored from a transient batch.
- Probe-only streaming must not allocate owned string bytes.
- Null string key semantics must match current fixed nullable key semantics.
