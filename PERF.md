# Performance Plan

## Benchmark baseline (TPC-H scale 10, orders.parquet, 15M rows)

Query: filter `o_totalprice > 400000 AND (o_custkey < 1000 OR o_custkey > 149000)`, project 4 columns.

| System | Time | Notes |
|--------|------|-------|
| PostgreSQL | 0.20s | native binary format, seq scan |
| **qdb** | **1.28s** | RelWithDebInfo, all columns materialized |
| pandas (vectorized) | ~0.30s | column pruning + numpy filter |
| Python (row-by-row) | 8.17s | pure Python loop |

Arrow raw read cost:
- All 9 columns: 0.708s
- 4 columns only: 0.169s — **4.2× faster from pruning alone**

---

## Root causes

### 1. Full column materialization (главная проблема)

`TParquetSource::Next()` читает и материализует все 9 колонок каждого батча, даже если фильтр использует только 2 (`o_totalprice`, `o_custkey`) и проект — 4.

Схема orders.parquet:
- `o_orderkey` int32 → нет в фильтре, нет в проекте на этапе фильтра
- `o_orderstatus` string → не нужна вовсе
- `o_orderdate` date32 → не нужна вовсе
- `o_clerk` string → не нужна вовсе
- `o_comment` string (самая тяжёлая) → не нужна вовсе

Каждая строковая колонка требует O(n) конвертации офсетов + heap alloc 512KB за батч.
229 батчей × 5 ненужных колонок = сотни MB аллокаций и сотни миллисекунд.

### 2. FLOAT → double widening ✅ исправлено

`FLOAT` колонки раньше копировались в `vector<double>` (O(n) + heap alloc).
Теперь zero-copy через `NumericData()`, аналогично всем остальным числовым типам.

### 3. Нет pushdown статистик Parquet

Arrow/Parquet хранит min/max на row group. Для фильтра `o_totalprice > 400000`
можно пропускать row groups где `max(o_totalprice) <= 400000` без чтения данных.

### 4. String offset conversion всегда O(n)

Для каждой строковой колонки конвертируем int32-офсеты в int64 даже если колонка не нужна.
Устраняется как следствие column pruning (пункт 1).

---

## Roadmap

### P1 — Column pruning (ожидаемый выигрыш: -0.5–0.7s)

**Что сделать:** плэннер вычисляет union колонок, нужных filter + project, и передаёт их индексы в `TParquetSource`.

```cpp
// TParquetSource: новый конструктор с column_indices
TParquetSource(const std::string& path, std::vector<int> columnIndices = {});

// Внутри: передаём индексы в Arrow
fileReader->GetRecordBatchReader(/*row_groups=*/{}, columnIndices, &Reader_);
```

Плэннер (`TPhysicalPlanner::Build`) собирает нужные колонки:
- Из предиката фильтра — имена полей, на которые ссылается ядро
- Из проекта — явный список

Логический оператор `TSourceOperator` хранит `vector<string> RequiredColumns_` (заполняется плэннером при обходе сверху вниз).

**Ожидаемое время после:** ~0.35–0.45s

### P2 — Parquet row group statistics pushdown (ожидаемый выигрыш: зависит от данных)

Для простых предикатов вида `col > val` или `col < val` проверять min/max статистику row group и пропускать группы без чтения.

```cpp
// В TParquetSource::TParquetSource: собрать row groups, прошедшие статистику
auto* metadata = FileReader_->parquet_reader()->metadata();
for (int rg = 0; rg < metadata->num_row_groups(); ++rg) {
    if (RowGroupMatchesPredicate(metadata->RowGroup(rg), predicate)) {
        rowGroups_.push_back(rg);
    }
}
fileReader->GetRecordBatchReader(rowGroups_, columnIndices, &Reader_);
```

Для TPC-H orders с фильтром `> 400000` часть row groups точно не пройдёт.

### P3 — SIMD vectorization проверка

Убедиться, что LLVM генерирует vectorized loop для фильтра. Добавить
`opts.PrintLlvm = true` в `TKernelCompiler` (при дебаг-запуске) и проверить наличие AVX/NEON инструкций в IR.

Если нет — добавить `loop.vectorize` hint в генерируемый IR или использовать
`clang_vectorize` attribute на сгенерированной функции.

### P4 — Kernel ABI: убрать heap alloc в dispatch

В `TKernelCompiler::CompileFilter` dispatch lambda создаёт `std::vector<uintptr_t> ctx`
на куче при каждом вызове (229 раз). Заменить на stack array:

```cpp
// Вместо: std::vector<uintptr_t> ctx; ctx.reserve(...);
uintptr_t ctx[kMaxColumns + 2];  // stack, не heap
```

Выигрыш небольшой (~5-10ms) но устраняет ненужное давление на аллокатор.

### P5 — Python pandas тест

Файл `build/complex_filter_project_pandas.py` добавлен.
Читает только нужные 4 колонки + numpy-vectorized filter.
Ожидаемое время: ~0.25–0.35s (сравнимо с PG).

---

## Ожидаемые результаты после P1+P2

| System | Оценка после оптимизаций |
|--------|--------------------------|
| PostgreSQL | 0.20s |
| qdb (P1+P2) | ~0.25–0.35s |
| pandas (P1) | ~0.25s |
