# Residual Join Filter — Kernel Injection (пошаговый план)

## Контекст / проблема

Сейчас residual-фильтр (например `(!= |l1.l_suppkey| |l2.l_suppkey|)` в Q21) применяется **после** материализации пар: `TRuntimeJoin::Next()` строит полный `TRowSet`, зовёт скомпилированный фильтр и возвращает batch с маской `Selection`. Это **семантически неверно** — операторы выше, которые не читают `Selection` (например `TRuntimeProject`), молча оставляют отфильтрованные строки.

Правильный уровень — **внутри ядра join, до `pb_push`**, ровно так же, как per-query собираются и инжектятся hash/equal функции ключа (`GenKeyOperationFunDecls` + `buildProgram` в `compiler.cpp:501`). Предикат компилируется в Qumir-функцию с фиксированным именем **`jt_residual_filter`**, инжектится в тот же LLVM-модуль, что и ядро, и вызывается из `jt_emit_and_insert`. Если фильтра нет — инжектится тривиальная always-true версия (после `-O3` inline + DCE стоит ноль).

**Решённый вопрос про доступ к данным:** колонки обеих сторон передаются в ядро **явно**. Каждый `TRowStore` отдаёт базу своего непрерывного массива `TRowSet` (`TRowStore::Data()`); `jt_process_left/right` получают `left_store`/`right_store` как `<ptr TRowSet>` и прокидывают их в `jt_emit_and_insert` → `jt_residual_filter`, которая декодирует упакованные row-id (`batch_idx<<32 | row_idx`) и индексирует `store[batch_idx].Columns[colIdx]`.

---

## Шаги исполнения

### Шаг 1 — `qdb/kernel/join/join_update.oz`
- [x] В цикле по совпадениям добавить `flt_l`/`flt_r` (упорядочены left,right по `is_left`) и обернуть `pb_push` в `(if (call jt_residual_filter ...) ...)`.
- [x] Добавить в `jt_emit_and_insert` параметры `left_store`/`right_store` (`<ptr TRowSet>`).
- [x] Передать `left_store right_store` в вызов `jt_residual_filter`.

### Шаг 2 — `qdb/kernel/join_gen.cpp` + `gen.h`/`gen.cpp`
- [x] `GenJoinProcessAst`: добавлены параметры `left_store`/`right_store` (`<ptr TRowSet>`), прокинуты в вызов `jt_emit_and_insert`.
- [x] ~~`GenJoinResidualFilterDefaultAst`~~ → дефолт вынесен в `.oz` (см. Шаг 8), generated-версия не нужна.
- [x] Новая `GenJoinResidualFilterAst(predicate, innerType, leftFieldCount, columnType, rowSetType, stringViewType)` в **`gen.cpp`** (рядом с `GenFilterKernelAst`, чтобы переиспользовать file-local хелперы):
  - декодирует `left_batch/left_row/right_batch/right_row`;
  - `left_cols = (field (index left_store left_batch) Columns)`; аналогично right;
  - связывает **только** упомянутые в предикате колонки (`CollectIdentNames`), по `leftFieldCount` выбирая сторону;
  - материализует через **`BuildColumnValueAst`**, повторяя паттерн `GenFilterKernelAst`: `SpecializeFilterPredicate`, `SubstFieldsInPlace`, `BuildFilterTruthAst`, `UsesNullableValue`;
  - вычисляет предикат, приводит к bool, `return`.
- [x] **Доп.: `CloneFilterExpr`** — `buildProgram` зовётся 5× (по entry), а `Specialize/Subst` мутируют предикат на месте; генератор клонирует вход в начале.

Сигнатура (дефолт и generated совпадают):
```
(var left_store <ptr TRowSet>) (var right_store <ptr TRowSet>)
(var left_row_id i64) (var right_row_id i64) -> bool
```

### Шаг 3 — `qdb/kernel/compiler.h` / `compiler.cpp`
- [x] `TJoinKernels::ProcessLeft/ProcessRight`: `std::function` расширена на два `TRowSet*` (базы сторов).
- [x] `CompileJoin`: новые опц. параметры `residualPredicate`, `innerType`, `leftFieldCount`.
- [x] В `buildProgram()`: при заданном предикате **подменяется** дефолтная `jt_residual_filter` из библиотеки на `GenJoinResidualFilterAst(...)` (с сохранением позиции). Для LeftSemi/LeftAnti с residual `kernelType = Inner`.
- [x] `TProcessFn` → `bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*)`; для `jt_insert_key_only` оставлен отдельный 5-арг `TInsertKeyOnlyFn`.

### Шаг 4 — `qdb/exec/join_exec.h`
- [x] `TRowStore`: добавлен `const TRowSet* Data() const`.
- [x] `TRuntimeJoin`: убраны `ResidualFilter_`, `ResidualSelBuf_`, `InnerOutputType_`, `InnerBuilder_`, `InnerLeftIds_`, `InnerRightIds_`, `DrainPairsToResidualVecs`; оставлены `MatchedLeftIds_`, `ResidualSemiAntiDone_`; добавлен `bool HasResidual_`.
- [x] Конструктор `TRuntimeJoin`: residual-параметры заменены на `bool hasResidual`.

### Шаг 5 — `qdb/exec/join_exec.cpp`
- [x] Конструктор: сохраняет `HasResidual_`; настройка `InnerBuilder_` удалена.
- [x] `PullOneInputBatch`: передаёт `LeftRows_.Data()` / `RightRows_.Data()` в `ProcessLeft`/`ProcessRight`.
- [x] LeftSemi/LeftAnti + residual: дренаж со сторами + `CollectMatchedLeftIds()`, затем emit из `MatchedLeftIds_`. `InnerBuilder_` убран.
- [x] INNER + residual: post-batch фильтр и `ResidualSelBuf_` убраны.
- [x] Остальные пути: добавлены store-аргументы.

### Шаг 6 — `qdb/exec/planner.cpp`
- [x] Убран `fc.CompileFilter(...)` / `residualDispatch`; в `CompileJoin` передаются `join->Filter()`, inner `TStructType*`, `leftType->Fields.size()`.
- [x] `hasResidual = (join->Filter() != nullptr)` передаётся в `TRuntimeJoin`.
- [x] Guard для residual на Left/Right/Full/RightSemi/RightAnti сохранён.

### Шаг 7 — `qdb/modules/qumirdb_runtime.h/.cpp`
- [x] Изменений нет (thread_local-трамплин отброшен).

### Шаг 8 — `qdb/kernel/join/join_residual_default.oz` (новый) + `lib.cpp`
- [x] Дефолтная always-true `jt_residual_filter` вынесена в отдельный `.oz`, подключена в `BuildJoinKernelLibrary()` **перед** `join_update.oz` → библиотека самодостаточна (исправляет `test_join_kernel`).
- [x] `test/test_join_kernel.cpp`: вызовы `ProcessLeft/Right` обновлены под новый ABI.

---

## Инварианты

| Свойство | Было (post-mat) | Станет (kernel injection) |
|---|---|---|
| Downstream видит `Selection` | Да (INNER) | Никогда |
| Корректность LeftSemi/LeftAnti | Случайная | По построению |
| Накладные при отсутствии фильтра | Ноль | Ноль (default inline + DCE) |
| Цена фильтра на пару | alloc batch + `TakeColumn` | Прямое индексирование, inline, без alloc |
| Стабильность указателей | — | `Data()` перечитывается на каждый вызов; row-id ссылаются только на уже добавленные batch'и |

---

## Проверка

```bash
cmake --build build -j
cd build && ctest --output-on-failure
../benchmark/tpch/run_tpch.sh ~/Projects/tpch 1 21
../benchmark/tpch/run_tpch.sh ~/Projects/tpch 1      # все 22
```

Ожидается: 22/22 проходят; `numwait` в Q21 совпадает с эталоном. Желательно добавить unit-тест join_exec: INNER join с residual-неравенством, где project выше видит только выжившие строки.

**Результат:** ✅ 19/19 unit-тестов и 22/22 TPC-H SF1 — зелёные; Q21 ≈ 1.5 с.
