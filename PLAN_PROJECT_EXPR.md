# План: вычисляемые колонки в Project (expression kernels) → путь к TPC-H Q1

## Context

Мотивация — **TPC-H Q1**: агрегаты над **выражениями**, а не над голыми колонками:
```sql
sum(l_extendedprice * (1 - l_discount))          AS sum_disc_price,
sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
avg(l_quantity) AS avg_qty, ...
```

Сейчас:
- **Project** — только passthrough по имени (`TIdentExpr`); планировщик на
  не-ident проекции бросает `"project expression kernels are not implemented yet"`
  (`exec/planner.cpp`). `project_exec.cpp` — zero-copy переиндексация колонок.
- **Aggregate** — `agg.Arg` парсится как выражение, но `CompileAggregate`
  требует, чтобы это была одна **общая** колонка-ссылка (`TIdentExpr`), и
  reducer-стейты — `i64` (не f64).

**Ключевая идея (и то, о чём спрашивал Царь):** вычисляемые колонки живут в
**Project** (SELECT-list оператор). Тогда:
- `sum(expr)` = project-**под**-aggregate вычисляет `expr` в колонку →
  `aggregate` суммирует обычную колонку;
- `avg(x)` = `aggregate` даёт `sum(x)` и `count(*)` → project-**над**-aggregate
  считает `avg = sum / count`. **Новая агрегатная функция не нужна.**

То есть одна фича — **вычисляемое выражение в project** — закрывает и
`sum(expr)`, и `avg`.

---

## Принцип: переиспользовать expression-ядро фильтра

`CompileFilter`/`GenFilterKernelAst` (`gen.cpp`) уже компилируют **произвольное
скалярное выражение над колонками**: bind всех входных колонок → материализация
через `BuildColumnValueAst` (Value/IsValid) → вычисление выражения → запись
`bool` в `selection[i]`. Project-ядро **отличается только выходом**: вместо одного
bool в `selection` оно вычисляет N выражений и пишет каждое в свою выходную
колонку. Весь column-bind + материализация + лоуэринг выражений переиспользуются.

```
filter:   for i: selection[i] = (u8) <predicate(cols, i)>
project:  for i: out_0[i] = <expr_0(cols, i)>;  out_1[i] = <expr_1(cols, i)>; ...
```

---

## P1 — вычисляемые колонки в Project (ядро фичи)

### P1.1 `CompileProject` (`kernel/compiler.{h,cpp}` + `gen.cpp`)
- Новый `CompileProject(inputType, projections)` → ядро, которое за один проход
  по батчу вычисляет каждое проекционное выражение и заполняет выходные колонки.
- Кодоген `GenProjectKernelAst` — по образцу `GenFilterKernelAst`: тот же
  column-bind + `BuildColumnValueAst`; в цикле для каждой проекции `j` —
  `out_j[i] = cast(<expr_j>, out_j_type)` (через `TArrayAssignExpr`, как
  `selection[i]=…` у фильтра).
- **Тип выходной колонки** — результирующий тип выражения. Берётся из
  qumir-лоуэринга (тот же type-resolve, что уже делает `CompileKernelAst` для
  фильтра): скомпилировать выражение, прочитать его `Type`. Для `f64*f64`→`f64`,
  целочисленных промоушенов и т.п. инференс делает компилятор qumir.
- ABI ядра: `void(*)(TRowSet* in, <out column buffers>)` — выходные буферы
  аллоцирует `TRuntimeProject` (как aggregate finalize аллоцирует свои).

### P1.2 Типизация (`pipeline/typing.cpp`)
- Ветка `TProjectOperator` сейчас: для `TIdentExpr` копирует тип поля, иначе тип
  `null`. Нужно **инференс типа выражения** над входной схемой (через qumir
  type-inference поверх входного `TStructType`), чтобы выходная схема была полной.
- Column pruning: `ComputeReferencedColumns` у project уже = unbound-vars
  проекций (работает для выражений — `FindUnboundVars` обходит дерево).

### P1.3 `TRuntimeProject` (`exec/project_exec.{h,cpp}`) — ГИБРИД zero-copy

> **Управляющее правило (по требованию Царя): невычислимые колонки остаются
> zero-copy, как сейчас.** Материализуются ТОЛЬКО вычисляемые колонки.

Выход — **по-колоночный гибрид**:
- проекция = `TIdentExpr` → выходная `TColumn` **указывает в колонку входного
  батча** (zero-copy), входной батч удерживается (`Retain`/хранится в `Private`,
  как сейчас `TProjectedRowSetData.Input`);
- проекция = выражение → выходная `TColumn` указывает в **owned-буфер**,
  заполненный ядром `CompileProject`.

`Destroy` освобождает input (Release) + owned-буферы. Если ВСЕ проекции ident —
текущий путь без изменений (ядро не компилируется). `CompileProject` компилирует
ядро **только для вычисляемых** колонок (читает нужные входные колонки → пишет
их owned-буферы); ident-колонки ядро не трогает. Селекшн входа пробрасывается на
выход (вычисляемые буферы заполняются по всем строкам, селекшн отбирает).

### P1.4 Planner / sexp
- `planner.cpp`: ветка project — если все проекции ident, текущий путь; иначе
  `CompileProject`. Убрать `throw "not implemented"`.
- sexp уже умеет `(<name> <expr>)` в `(rel project … (disc (* a (- 1 b))))`
  (парсер проекций берёт произвольный `h.Expr()`).
- Тесты: вычисляемые колонки (арифметика int/f64, литералы, вложенность) vs
  ручной расчёт; pruning сужает вход к используемым колонкам.

> Это и есть «генерируемые колонки». Дальше — что нужно ДОПОЛНИТЕЛЬНО именно для Q1.

---

## Карта зависимостей до полного Q1

Q1 после декомпозиции (вся «магия выражений» — в двух project'ах):
```
project(                     ; P4: avg_* = sum_* / count  (+ passthrough)
  aggregate(                 ; P2+P3: суммы по РАЗНЫМ f64-колонкам
    project(                 ; P1: disc_price, charge
      filter(source lineitem, l_shipdate <= <литерал-дата>),   ; P5
      l_returnflag l_linestatus l_quantity l_extendedprice l_discount
      (disc_price (* l_extendedprice (- 1 l_discount)))
      (charge (* (* l_extendedprice (- 1 l_discount)) (+ 1 l_tax)))),
    keys (l_returnflag l_linestatus)
    (sum_qty sum l_quantity) (sum_base_price sum l_extendedprice)
    (sum_disc_price sum disc_price) (sum_charge sum charge)
    (sum_disc sum l_discount)            ; нужен только для avg_disc
    (count_order count)),
  l_returnflag l_linestatus sum_qty sum_base_price sum_disc_price sum_charge
  (avg_qty (/ sum_qty count_order)) (avg_price (/ sum_base_price count_order))
  (avg_disc (/ sum_disc count_order)) count_order)
```

| Шаг | Что нужно | Зачем для Q1 | Статус |
|---|---|---|---|
| **P1** | вычисляемые колонки в project | `disc_price`/`charge` под sum; `avg=sum/count` сверху | план |
| **P2** | aggregate по **нескольким разным** колонкам-аргам (сейчас одна общая) | Q1 суммирует 5 разных колонок | нужно |
| **P3** | **f64**-агрегаты (reducer-стейты f64, не i64) | `l_extendedprice/discount/tax` — double | нужно |
| **P4** | `avg` = project(sum/count) **сверху** | три `avg_*` | = P1 (reuse) |
| **P5** | дата-литерал в фильтре (`date '1998-12-01' - interval 81 day` → константа `int32` дней; `l_shipdate` — `date32`) | `WHERE l_shipdate <= …` | мелочь (precompute в plan-build) |
| **сорт** | `ORDER BY` | детерминированный вывод | shell-сортировка (как в существующих `.sh`), отдельный sort-оператор — позже |

**Наблюдение:** P4 (avg) и `sum(expr)` — это **целиком** P1 + обычные суммы.
Реальные «новые» куски для Q1, помимо P1 — это P2 (мульти-колоночный агрегат) и
P3 (f64-агрегаты). Дата (P5) — константа, сорт — в шелле.

---

## Статус

- **P1 — ГОТОВ ✅** (вычисляемые колонки в project, гибрид zero-copy):
  - **P1a** `InferProjectExprType` (`kernel/project_type.{h,cpp}`) — отдельный
    инференс-проход; уважает явные аннотации `(: x T)`/cast, консервативный
    f64-промоушен. 7 тестов.
  - **P1b** `GenProjectKernelAst` (`gen.cpp`, reuse фильтр-механики:
    `SpecializeFilterPredicate`/`SubstFieldsInPlace`/`BuildColumnValueAst`) +
    `CompileProject` (`compiler.{h,cpp}`): пишет `out[k][i]=cast(expr_k, T_k)`.
    Тест: f64 `p*(1-d)` + i64 `k+100`.
  - **P1c** `TRuntimeProject` ГИБРИД: ident-колонки zero-copy (указывают в
    удержанный input-батч), только computed материализуются в owned-буферы;
    `typing.cpp`/`planner.cpp` используют инференс. e2e-тест (вкл. проверку
    zero-copy `Data`) + CLI на lineitem — `l_extendedprice*(1-l_discount)`
    **побайтово совпал с DuckDB**.
  - 18/18 ctest.
- **P3 — ГОТОВ ✅** (f64-агрегаты, non-nullable):
  - Идея: f64 и i64 — оба 8 байт, `AggBuffers` хранит f64-биты как i64 →
    **layout/`aht_init`/`aht_rehash` НЕ меняются**, только редьюсер реинтерпретирует
    биты. Добавлен runtime `qdb_bits_f64(u64)->f64` (обратный к `qdb_f64_bits`).
  - `TAggReducerInfo.IsFloat`; `GenReducerFunDecls` Shape A f64-ветка
    (`prev_f=qdb_bits_f64(prev)` … `return qdb_f64_bits(result)`); dispatch
    переносит f64-значение как биты (`qdb_f64_bits(values[i])`); `CompileAggregate`
    принимает f64-арг. i64-путь байт-идентичен (`IsFloat` по умолчанию false).
  - Тест на точных значениях (sum 4.0/10.25 точно, min/max точны). CLI на
    lineitem: min/max совпали с DuckDB точно, sum — в пределах f64-неассоциативности
    (порядок суммирования ~6M значений).
  - **Ограничение:** nullable-f64 пока не поддержан (Shape B i64-only) — отдельно.
    19/19 ctest.
- **P2 — ГОТОВ ✅** (агрегат по нескольким разным колонкам):
  - Подход (согласован): **унифицированный инлайн** редьюсеров в dispatch с
    per-reducer значениями. `reduce_i`-функции не тронуты.
  - `TAggReducerInfo.ArgColumnIndex` + `TAggArg`; `BuildAggReducerLayout(funcs,
    args)`; `CompileAggregate` снял single-column ограничение. Dispatch
    материализует каждую **различную** колонку (`values_<idx>`/`arg_column_<idx>`),
    считает `arg_val_<idx>` раз на строку, инлайнит применение редьюсеров
    (`buf_i[slot]=reduce_i(...)` с per-reducer value/valid). `agg_apply_reducers`
    больше не зовётся из dispatch.
  - **Перф:** single-column hot path не регрессировал — `filter_aggregate_string.sh`
    на pq100: `Processed ~3.87s` (baseline ~3.9-4.0s), LLVM O3 оптимизирует инлайн
    эквивалентно вызову.
  - Тесты: мульти-колонка точные значения (sum/max/count разных колонок); CLI на
    lineitem (`sum(l_quantity/extendedprice/discount), count` по `l_returnflag`)
    vs DuckDB — count/целочисленные суммы точны, f64-суммы в пределах
    неассоциативности. 19/19 ctest.

## Рекомендация по порядку

1. ~~**P1** — вычисляемые колонки в project~~ — **сделано**.
1. **P1** — вычисляемые колонки в project (headline-фича, переиспользует
   expression-ядро фильтра). Сразу проверяется арифметикой + `avg=sum/count`
   поверх существующего i64-агрегата.
2. **P3** — f64-агрегаты (расширить reducer-layout/кодоген на f64-стейты).
3. **P2** — мульти-колоночный агрегат (per-agg arg вместо одной общей колонки).
4. **P5 + сорт** — дата-литерал + shell-сортировка → собрать **полный Q1**,
   сверить с DuckDB (`tpch_q1.sh` + `..._duckdb.sh`, как join_filter_aggregate).

Каждый шаг дробится на мелкие (как D–H / K1–K4) при реализации.

## Решения (согласовано)
- **Инференс типов выражений** — **отдельным type-inference проходом** (не из
  лоуэринга): явный API инференса по входной `TStructType`.
- **NULL-семантика** — **Stage 1 без nullable** (Q1-колонки non-null); позже —
  «по-нормальному»: nullability из типа данных в source (как уже в агрегатах).
- Типы датасета: `l_quantity/extendedprice/discount/tax` — **double**, ключи
  `l_returnflag/l_linestatus` — **string** (агрегат умеет), `l_shipdate` —
  **date32[day]**. Все `avg_*` — `f64`; **P3 обязателен**.

## Оценка: generic-редьюсеры (функция прямо в agg-ноде)

Идея Царя: вместо зашитых `count/sum/min/max` задавать редьюсер в ноде, напр.
`(agg c (fun (prev cur) -> (if (< cur prev) cur prev)) v)`.

**Правки Qumir? — в основном НЕТ.** Тело редьюсера — обычное выражение над
`prev`/`cur`(/validity), такие Qumir уже компилирует (фильтр/project).
`GenReducerFunDecls` (`gen.cpp`) уже **строит тела reduce-функций программно**
(сейчас по `switch(func)`); обобщение = взять тело **из AST ноды** вместо
хардкода — сплайсинг выражения, не новая фича языка. Расширять надо лишь
**sexp-парсер qdb**, не ядро Qumir.

**Реально сложное (дизайн, не Qumir):**
- **init/identity и «первое значение»** (`sum`→0; `min/max`→первое). Чистый
  контракт: `(init_expr, combine_expr(prev, value, valid))`, первое = `combine(init,…)`.
- **null** в combine (см. текущие nullable-редьюсеры).
- **multi-state** (`avg`, `stddev`) — одно `(prev,cur)→new` их не выражает; нужен
  project-сверху (наш план для avg) или объявление ширины состояния.

**Вердикт: отложить.**
- **Для Q1 НЕ нужно** — `sum/avg/count` над f64 закрываются текущими функциями +
  `avg=project(sum/count)`. Generic-редьюсеры Q1 не разблокируют.
- В целом — правильный generic-дизайн (в духе reuse, как `<named Key>` в join),
  но это **рефактор перф-чувствительного агрегатного пути** (nullable, перф-гейты)
  → риск регрессий. Делать **отдельным планом** `PLAN_AGG_REDUCERS.md` ПОСЛЕ Q1.
- **Дешёвый промежуточный шаг (без ABI-ломки):** вынести тела редьюсеров в
  **таблицу** `(func → init, combine)` в `gen.cpp` — добавить функцию (`product`,
  нативный `avg`) станет одной строкой, без правок shape-логики; user-defined в
  ноде — поверх, когда понадобится. Перф-гейт `filter_aggregate_string.sh` —
  стандартные функции должны остаться байт-идентичными.

## Проверка
- P1: вычисляемые колонки vs ручной расчёт (unit) + e2e через planner.
- Полный Q1: `tpch_q1.sh` (qdb) побайтово совпадает с `tpch_q1_duckdb.sh` после
  сортировки по `(l_returnflag, l_linestatus)`.
