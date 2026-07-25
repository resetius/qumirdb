# План: оператор Join (симметричный потоковый hash join + Robin Hood)

## Context

Цель — реляционный оператор `Join` (SQL `INNER`/`LEFT`/`RIGHT`/`FULL JOIN` по
equi-условию `left.k = right.k`) поверх **симметричного потокового хэш-джойна**
(symmetric / pipelined hash join, Wilschut & Apers): два независимых Robin-Hood
хэш-стола, один на сторону. Каждая входная строка **пробит ПРОТИВОПОЛОЖНУЮ**
таблицу (эмитит совпадения), затем безусловно **вставляется в СВОЮ** таблицу.

Корректность доказана в обсуждении (для любой совпадающей пары `(l, r)` —
какая бы сторона ни пришла позже, она найдёт уже вставленную раньше сторону →
ровно одна эмиссия на пару, без дублей и пропусков) и реализована как C++
прототип-oracle в `test/test_join.cpp`:
- `InnerJoin(left, right)` — сам симметричный алгоритм (probe-opposite /
  insert-own, батчами по 10 строк, round-robin между сторонами).
- `NestedLoopInnerJoin(left, right)` — brute-force oracle.
- `CheckJoinMatchesNestedLoop` + 6 тестов (`BasicInnerJoin`,
  `LargeTablesManyKeys`, `FewKeysHighCardinality`, `SingleKeyFullCrossProduct`,
  `AsymmetricSidesAndKeyRanges`, `EmptySides`) — все проходят.

`test/test_join.cpp` — это **референс-спецификация алгоритма**, а не часть
`qdb`. Реализация в этом плане воспроизводит тот же алгоритм в виде JIT-ядра
(oz-lang), а `GenerateTable`/`NestedLoopInnerJoin`/`CheckJoinMatchesNestedLoop`
переиспользуются как генератор входных данных и oracle для e2e-тестов
`TRuntimeJoin` (через `TRowSet`, а не `std::map`).

**LEFT/RIGHT/FULL OUTER** — единый приём **"deferred final scan"**: основной
потоковый цикл (probe-opposite / insert-own) не меняется; после исчерпания
ОБОИХ входов делается один финальный проход по dense-слотам одной таблицы —
для каждого ключа `k`, у которого `rh_lookup_slot(opposite, k) == -1` (т.е. ни
одна строка противоположной стороны НИКОГДА за весь стрим не имела такого
ключа), эмитятся все строки `own.RowBucket[k]` с NULL-заполнением
противоположных колонок. `FULL OUTER` = финальный проход в обе стороны.
Условие `rh_lookup_slot == -1` ровно покрывает "никогда не совпало" — без
повторной эмиссии уже сматченных в стриме пар. Прямой аналог —
finalize-проход по dense-слотам в Aggregation
(`aggregation_finalize_generic.oz`).

**Grace-партиционирование** — НЕ часть Stage 1-3. По итогам анализа реального
YDB `GraceJoin` (`mkql_grace_join_imp`) бакетирование по `hash(key) % N` —
**алгоритм-агностичная обёртка** над любым join-ядром (co-location +
memory-bounding + spill granularity + bloom-based early rejection). ABI-вывод:
ядро проектируется как `<ref JoinState>`-на-партицию (зеркалит `<ref
HashTable>` агрегации), так что переход N=1 → N>1 — чисто аддитивен (массив N
состояний + router + per-partition spill). См. "Stage 4 — намётки".

**Этапность:**
- **Stage 1** — `INNER JOIN`, один equi-key `i64` (scalar), оба входа целиком в
  памяти (без spill). Аналог Aggregation Stage 1 (`Key=i64` concrete) — **не
  блокируется** на раздел **M** `PLAN_AGGREGATION.md` (generic key pipeline,
  сейчас в паузе и build RED); может стартовать независимо на том же
  concrete-i64 фундаменте.
- **Stage 2** — Row Store: материализация строк через `TRowSet::Retain` +
  packed `RowId`; growable row-id bucket как dense payload хэш-таблицы (новая
  инфраструктура, которой нет в Aggregation).
- **Stage 3** — `LEFT`/`RIGHT`/`FULL OUTER` через deferred final scan.
- **Stage 3b** — `LEFT`/`RIGHT SEMI` (потоковый, emit-once на строку) и
  `LEFT`/`RIGHT ANTI` (блокирующий, только final scan). Переиспользуют тот же
  `rh_lookup_slot(opp,k) == -1` примитив и финальный dense-проход, что и OUTER:
  ANTI — это «NULL-часть» OUTER без потоковой эмиссии, SEMI — inner с дедупом
  по строке. См. "Матрица вариантов Join" и "Stage 3b — намётки".
- **Stage 4** — generic equi-key (composite / `i32`/`f64`/...) — сходится с
  Aggregation's `<named Key (template readable mutable)>` + `rh_hash`/
  `rh_key_equal` overload-контрактом (раздел M там же).
- **Stage 5 (намётки/future)** — Grace partitioning + spill, строковые/large
  значения, `MARK`/`SINGLE` join (флэттенинг подзапросов), а также in-kernel
  residual `θ(left,right)` для двусторонних не-equi конъюнктов ON (см. локус
  фильтра — в текущий план НЕ вносится, отложено).

**Руководящий принцип — максимальное переиспользование инфраструктуры
Aggregation.** Join не строит свой стек хэширования: он переиспользует
Robin-Hood `HashTable`, её lifecycle (`aht_init`/`aht_rehash`/`aht_destroy`),
generic-probe (`rh_lookup_slot`/`rh_insert_displace`/`rh_rehash_into`),
`<named Key>`-инстанциацию + overload-контракт (`rh_hash`/`rh_key_equal`),
а в Stage 4 — и descriptor (`BuildAggregateKeyDescriptor`/`RepresentKeyType`) +
column-read кодоген (`gen.cpp`). Любой новый join-код сперва проверяется на
«нельзя ли это взять из `qdb/kernel/aggregation/` или `gen.cpp`». Расхождение
допустимо только там, где семантика реально иная: dense-payload (RowId-бакеты
vs reducers) и эмиссия пар. См. "Stage 4 — намётки" (карта переиспользования).

**Текущий статус:** **Stage 1 (INNER, i64-ключ, in-memory) — ГОТОВ** (см.
"Stage 1 — ГОТОВ" внизу): полный путь sexp → typing → pruning → planner →
`CompileJoin` (JIT поверх переиспользованной `HashTable`) → `TRuntimeJoin` →
gather-выход; e2e + CLI на parquet, 14/14 ctest. Этот документ — план; шаги
помечаются `[x]` и снабжаются `> Реализация:` блоками (как в
`PLAN_AGGREGATION.md`).

---

## Матрица вариантов Join и локус фильтра (анализ)

Симметричная схема работает **только для equi-join** (нужен конъюнкт
`l.k = r.k` для хэширования). Колонка «filter в ядре» — нужен ли
**компилируемый residual `θ(left,right)`** внутри `jt_process_batch` (проверка
в точке матча, до NULL-дополнения / до решения о «существовании»); одиночные
предикаты на одну сторону сюда не входят — они спускаются планировщиком в
сканы.

| Вариант | Equi? | Symmetric HJ | Filter в ядре (residual θ) | Этап | Примечания (TPC-H/DS) |
|---|---|---|---|---|---|
| INNER | да | ✅ | нет — perf-опция (≡ post-filter) | Stage 1 | TPC-H **Q19**, **Q7**; TPC-DS **Q72** — θ на обе стороны (PG: *Join Filter*) |
| LEFT OUTER | да | ✅ | **да** для бинарного θ (ON ≠ WHERE) | Stage 3 | TPC-H **Q13** — предикат в ON; перенос в WHERE демотит LEFT→INNER |
| RIGHT OUTER | да | ✅ | **да** для бинарного θ | Stage 3 | симметрично Q13 |
| FULL OUTER | да | ✅ | **да** для бинарного θ | Stage 3 | редко в TPC-*; важна корректность ON-θ |
| LEFT SEMI | да | ✅ | **да** — θ и есть тест существования | Stage 3b | TPC-H **Q21**; TPC-DS **Q94** (`<>`-residual) |
| RIGHT SEMI | да | ✅ | **да** | Stage 3b | симметрично |
| LEFT ANTI | да | ✅ (вырожд.) | **да** — θ определяет «не совпало» | Stage 3b | TPC-H **Q21**/**Q22**; TPC-DS **Q16**/**Q95** |
| RIGHT ANTI | да | ✅ (вырожд.) | **да** | Stage 3b | симметрично |
| MARK | да | ✅ | **да** (метка = θ-существование) | Stage 5 | дизъюнктивные `EXISTS`/`IN` (OR-флэттенинг) |
| SINGLE/scalar | да | ✅ | да (residual) + проверка кардинальности | Stage 5 | скалярные подзапросы `= (select ...)` |
| CROSS | нет | ❌ | n/a (отдельный NLJ) | вне SHJ | — |
| θ / non-equi | нет | ❌ | n/a (NLJ / sort-merge band) | вне SHJ | band-join, `between` по двум таблицам |
| ASOF | нет | ❌ | n/a (sort-merge / спец) | вне SHJ | временной «ближайший» |

\* SEMI — потоковый, но **со state**: per-left-row флаг «уже сэмичен»
(emit-once), правая сторона хранит только ключи. **ANTI — блокирующий**:
«нет совпадения» решается лишь после исчерпания противоположной стороны.

### Локус фильтра: «после» ≠ «внутри» (канонически — LEFT JOIN)

Сохраняемая сторона = left; NULL-дополняемая = right.

| Предикат на | INNER | OUTER | SEMI / ANTI |
|---|---|---|---|
| сохраняемую сторону | push в скан ≡ post (perf) | push в скан сохраняемой ≡ post (perf) | push во внешний скан (безопасно) |
| NULL-сторону | push в скан ≡ post (perf) | post-WHERE тихо демотит LEFT→INNER; right-only ON ≡ `σ_p(R)` до join — **разные операции** | сужает «существование» — осознанный выбор |
| **обе стороны (θ)** | residual ≡ post (perf) | **обязан в ядре** (ON-θ ≠ post-WHERE) | **обязан в ядре** (θ = определение матча) |

**Решение:** одиночные предикаты спускаются планировщиком в сканы (с оговоркой
про NULL-сторону под OUTER — это выбор ON vs WHERE). **Неустранимый** случай —
бинарный `θ(left,right)` под OUTER/SEMI/ANTI/MARK: его нельзя выразить
post-filter'ом и нельзя спустить в одиночный скан. Его ABI (компилируемый
предикат над парой схем в `jt_process_batch`/`CompileJoin`) **в текущий план НЕ
вносится — отложено** (Stage 5 намётки).

### Бенчмарки: где важен фильтр внутри join

В PostgreSQL такие предикаты появляются как **`Join Filter`** на узле join (а
не отдельный `Filter` сверху). Push в ядро отсекает пары до материализации
выхода, а для OUTER/SEMI/ANTI ещё и обязателен для корректности. (Номера — по
памяти, стоит сверить с текстами запросов.)

| Запрос | Тип join | Residual-предикат | Почему важен в ядре |
|---|---|---|---|
| TPC-H **Q19** | INNER | большой `OR` над `(p_brand,p_container,p_size)` × `(l_quantity,l_shipmode,l_shipinstruct)` | двусторонний; канонический *Join Filter* на `lineitem ⋈ part` — push отсекает пары до выхода |
| TPC-H **Q7** | INNER | `(n1=X ∧ n2=Y) ∨ (n1=Y ∧ n2=X)` через два nation-джойна | **разбирается без residual в ядре**: вывести односторонние необходимые условия `n1∈{X,Y}`/`n2∈{X,Y}` → push в сканы `nation`, полный `OR` — пост-фильтром (INNER ⇒ ≡). Слабый пример «filter в ядре» |
| TPC-H **Q13** | LEFT OUTER | `o_comment not like '%...%'` **в ON** | односторонний, но в ON: WHERE-вариант демотит LEFT→INNER (классика ON≠WHERE). Спускается в скан `orders` |
| TPC-H **Q21** | SEMI + ANTI | `l3.l_suppkey <> l1.l_suppkey` (self-join `lineitem` по `l_orderkey`) | `<>`-residual = часть теста `EXISTS`/`NOT EXISTS` |
| TPC-H **Q22** | ANTI | `NOT EXISTS` по `o_custkey=c_custkey` (без θ) | чистый anti — для контраста: residual НЕ нужен |
| TPC-DS **Q72** | INNER | `inv_quantity_on_hand < cs_quantity` (`inventory ⋈ catalog_sales`) | двусторонний `<` (inequality join) |
| TPC-DS **Q16** | ANTI | `cs_warehouse_sk <> cs_warehouse_sk` (`catalog_sales`) | `<>` внутри `NOT EXISTS` |
| TPC-DS **Q94/Q95** | SEMI/ANTI | `ws_warehouse_sk <> ws_warehouse_sk` (`web_sales`) | то же на `web_sales` (Q94 — exists, Q95 — not-exists) |

**Разложение дизъюнкции (INNER).** Для двустороннего `D = ⋁ᵢ (Aᵢ ∧ Bᵢ)`
выполняется `D ⊨ (⋁ᵢ Aᵢ)` и `D ⊨ (⋁ᵢ Bᵢ)` — односторонние **необходимые**
условия спускаются в сканы соответствующих сторон, а полный `D` применяется
**повторно после join** (необходимое ≠ достаточное). Для INNER post-filter ≡
residual, поэтому **ядро не требуется** — это снимает Q7 (и частично Q19) из
мотивации kernel-residual. Неустранимы только OUTER (ON-θ) и SEMI/ANTI (θ —
определение матча, напр. `<>` в Q21): там post-filter семантически неверен.

---

## Stage 3b — намётки (SEMI / ANTI)

- `EJoinType` += `LeftSemi, RightSemi, LeftAnti, RightAnti`.
  `ComputeJoinOutputType`: выход = колонки **только** сохраняемой стороны
  (semi/anti не добавляют колонок противоположной стороны и не делают их
  nullable).
- **SEMI (потоковый, emit-once):** правая таблица хранит только ключи (RowBucket
  не нужен). При приходе left-строки `L(k)`: если `rh_lookup_slot(right,k) != -1`
  → эмитим `L` сразу (не буферизуем); иначе буферим `L` в `left.RowBuckets[k]`.
  При приходе right-строки `R(k)`: вставляем ключ в right-таблицу; если слот
  **новый** (первое появление `k` справа) → drain `left.RowBuckets[k]`, эмитим
  все накопленные left-строки и очищаем бакет. Последующие right того же `k` —
  бакет уже пуст. Зеркально для RIGHT SEMI.
- **ANTI (блокирующий):** основной цикл только строит таблицы (left хранит
  строки, right — ключи), без эмиссии. После исчерпания **обоих** входов —
  единственный final scan по `left.GroupKeys[0..left.Size)`: для каждого `i`,
  где `rh_lookup_slot(right, left.GroupKeys[i]) == -1`, эмитим все `rowId` из
  `left.RowBuckets[i]`. Это ровно deferred-scan-часть OUTER без потоковой части.
- `CompileJoin` op-коды: SEMI переиспользует `op==1/2` (process batch) с
  изменённой эмиссией; ANTI — `op==1/2` только строят, эмиссия в `op==4`
  (final scan), как OUTER.
- e2e-oracle: `NestedLoopInnerJoin` из `test/test_join.cpp` тривиально
  адаптируется в semi/anti-oracle (для каждой left-строки — `any`/`none`
  совпадений справа).

---

## Правила изменений `externals/qumir`

Действуют те же правила, что в `PLAN_AGGREGATION.md` (повторены для
самодостаточности):

- [ ] По умолчанию qdb адаптируется к существующим parser/runner/lowering API
  и их текущему поведению.
- [ ] В `externals/qumir` допустимы только минимальные локальные изменения, не
  меняющие поведение существующих программ и тестов: additive option, узкий
  bugfix или недостающий API с сохранением старого контракта.
- [ ] Нельзя без отдельного согласования менять entry point, порядок функций,
  ABI JIT-вызова, parsing/type annotation/lowering или другие существующие
  семантические контракты.
- [ ] Любая правка с заметным blast radius сначала выносится в отдельный
  воспроизводимый тест и согласовывается отдельно.
- [ ] Согласованная правка сабмодуля = отдельный коммит в `externals/qumir` +
  отдельное обновление указателя сабмодуля в qdb.

**Ожидание:** для Stage 1-3 новых изменений сабмодуля не требуется —
`AllowOverloads` и фикс unsigned bitops уже сделаны для Aggregation и
переиспользуются как есть (см. `qumir/runner/runner_llvm.{h,cpp}`,
`TLLVMRunnerOptions::AllowOverloads`).

---

## Архитектурные опорные точки (из исследования)

- **Оператор** = `IOperator : TExpr` (`qdb/ops/operator.h`). Несёт `Type =
  TFunctionType([requiredInput...], output)`; `Children()` уже возвращает
  `vector<TExprPtr>` — у Filter/Project/Aggregate ровно один элемент.
  **`TJoinOperator` — первый оператор с ДВУМЯ детьми** (`Left()`, `Right()`).
- **Sexp**: `qdb/sexp/printer.cpp` (`PrintRel`), `qdb/sexp/parser.cpp`
  (`MakeRelParsers`) — диспетчеризация по `RelName()`/`OpId`. Образец для
  multi-child + nested-clause синтаксиса: `(rel aggregate <input> (keys ...)
  (agg ...) ...)`.
- **Pipeline**:
  - `pipeline/typing.cpp::AnnotateTypes` — bottom-up; для Join: `ParamTypes =
    [left.ReturnType, right.ReturnType]`, `ReturnType =
    ComputeJoinOutputType(...)`.
  - `pipeline/column_pruning.cpp` — сейчас рассчитан на **один** `Input()`
    (top-down `walk(node.Input(), required)`). Join требует **разных**
    required-наборов для левого и правого ребёнка (join-ключ своей стороны ∪
    пересечение родительского required с колонками своей стороны). См. шаг C.
  - `pipeline/unbound_vars.cpp` — без изменений, переиспользуется.
- **Kernel**: `kernel/gen.cpp` (генерация AST ядра по runtime-схеме),
  `kernel/compiler.cpp` (`CompileFilter`/`CompileAggregate` — образец
  `CompileJoin`), `kernel/lib.{h,cpp}` (`ParseFunctionLibrary`/
  `MergeKernelLibrary` — слияние нескольких `.oz`-файлов в один AST).
- **Exec**: `IRuntimeNode { OutputType(); Next(TRowSet&) }`
  (`exec/executor.h`). Aggregate — pipeline breaker "впитать всё → отдать
  один раз". **Join — pipeline breaker иного рода**: симметричный алгоритм
  эмитит результаты ПО ХОДУ обработки (не только в конце), поэтому
  `TRuntimeJoin::Next()` — pull-loop `while (pairs empty) { progress(left) ||
  progress(right) }`, как `test_join.cpp`'s `progress`/`while`.
- **Данные**: `io/io.h` — `TColumn{Data,Mask,Offsets,...}`, `TRowSet{Columns,
  ColumnCount,RowCount,Selection,Destroy,Private,RefCount}` +
  `Retain(TRowSet*)`/`Release(TRowSet*)`. **Ключевая находка**: `Retain`/
  `Release` уже дают ref-counted владение батчем — Row Store переиспользует их
  напрямую вместо копирования колонок (см. шаг D).
- **Модуль**: `qdb/modules/qumirdb.cpp` (`ExternalTypes_`/`ExternalFunctions_`),
  `qdb/modules/qumirdb_runtime.cpp` (`qdb_alloc`/`qdb_realloc`/`qdb_free` —
  уже добавлены для Aggregation, переиспользуются как есть для growable
  row-id buckets / pair buffers).
- **Robin Hood инфраструктура** (`qdb/kernel/aggregation/`):
  - `aggregation_hashtable_generic.oz`: `rh_lookup_slot`/`rh_insert_displace`
    над `<named Key (template readable mutable)>` + sparse
    `keys[]`/`dist[]`/`slot_ids[]` (open addressing, dist=-1 ⇒ пусто) → dense
    `SlotId`. **Переиспользуется без изменений.**
  - `key_ops_i64.oz`: `rh_hash(i64)`/`rh_key_equal(i64,i64)` —
    **переиспользуется как есть** для Stage 1 (`Key=i64`).
  - Dense payload **отличается**: Aggregation хранит в dense-слоте
    `AggBuffers` (фиксированный `NumAggs*8` байт agg-state). Join хранит
    **growable bucket строковых RowId** (1:N/N:M cardinality) — см. шаг E.

---

## Открытый вопрос: multi-input операторы в pipeline

`TJoinOperator` — первый оператор с двумя входами. `column_pruning.cpp`'s
`walk(node, required)` сейчас вызывает `walk(node.Input(), required)`.
Минимальное обобщение (шаг C):

```cpp
// IOperator: дефолт для single-input операторов = текущее поведение.
virtual std::unordered_set<std::string> RequiredColumnsForChild(
    size_t childIdx, const std::unordered_set<std::string>& requiredFromParent) const {
    // childIdx==0 для всех текущих (single-input) операторов
    return ComputeReferencedColumns() | requiredFromParent; // текущая формула
}
```

`TJoinOperator` переопределяет:
```cpp
std::unordered_set<std::string> RequiredColumnsForChild(size_t i, const auto& parentRequired) const override {
    auto& sideCols = (i == 0) ? Left()->OutputColumns() : Right()->OutputColumns();
    auto& key = (i == 0) ? LeftKey_ : RightKey_;
    std::unordered_set<std::string> req = {key};
    for (auto& c : parentRequired) if (sideCols.contains(c)) req.insert(c);
    return req;
}
```

`column_pruning.cpp::walk` обобщается: `for (i, child) : enumerate(node.Children())
walk(child, node.RequiredColumnsForChild(i, required))`. Для существующих
single-input операторов поведение не меняется (дефолтная реализация = старая
формула, `childIdx` игнорируется).

---

## Stage 1 — реализация (INNER JOIN, `i64` scalar key, in-memory)

### 1. Логический оператор — `qdb/ops/join.{h,cpp}`

```cpp
enum class EJoinType { Inner /*, Left, Right, Full — Stage 3 */ };

class TJoinOperator : public IOperator {
public:
    static constexpr const char* OpId = "join";

    TJoinOperator(TOperatorPtr left, TOperatorPtr right,
                   std::string leftKey, std::string rightKey, EJoinType type);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override; // {} — handled via RequiredColumnsForChild
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t i, const std::unordered_set<std::string>& parentRequired) const override;
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Left_, Right_}; }
    const std::string ToString() const override;

    TOperatorPtr Left() const { return Left_; }
    TOperatorPtr Right() const { return Right_; }
    const std::string& LeftKey() const { return LeftKey_; }
    const std::string& RightKey() const { return RightKey_; }
    EJoinType Type() const { return Type_; }

private:
    TOperatorPtr Left_, Right_;
    std::string LeftKey_, RightKey_;
    EJoinType Type_;
};

// Output schema = left.OutputColumns() ++ right.OutputColumns().
// Stage 1: ошибка (NQumir::TError), если имена колонок пересекаются —
// никакого автоматического префиксирования/алиасинга пока нет.
std::expected<NQumir::NAst::TTypePtr, NQumir::TError> ComputeJoinOutputType(
    const NQumir::NAst::TTypePtr& left, const NQumir::NAst::TTypePtr& right, EJoinType type);

// leftKey/rightKey — имена колонок; обе должны быть i64 (Stage 1 ограничение,
// проверяется здесь как NQumir::TError).
std::expected<TOperatorPtr, NQumir::TError>
MakeJoin(TOperatorPtr left, TOperatorPtr right,
    std::string leftKey, std::string rightKey, EJoinType type);
```

### 2. Sexp — `qdb/sexp/{printer,parser}.cpp`

```
(rel join <left> <right>
  (on left_col right_col)
  (type inner))
```
- `(type ...)` опционален в Stage 1 (по умолчанию `inner`); в Stage 3
  добавляются `left`/`right`/`full`.
- `MakeRelParsers`: ветка `nameTok.Name == TJoinOperator::OpId` — по образцу
  aggregate: `co_await h.Expr()` для left, затем для right, затем
  `(on a b)`, опционально `(type ...)`.
- `PrintRel`: симметричная печать.

### 3. Pipeline — typing / pruning

- `pipeline/typing.cpp::AnnotateTypes`: ветка `TMaybeOp<TJoinOperator>` —
  `ParamTypes = [left.ReturnType, right.ReturnType]`, `ReturnType =
  ComputeJoinOutputType(...)`.
- `pipeline/column_pruning.cpp`: обобщение на N children (см. "Открытый
  вопрос" выше) + `TJoinOperator::RequiredColumnsForChild`.
- `unbound_vars.cpp` — без изменений.

### 4. Row Store — `qdb/exec/join_exec.{h,cpp}` (чистый C++, без oz)

Join должен сохранять ФАКТИЧЕСКИЕ СТРОКИ (не только ключи/агрегаты), чтобы
эмитить их позже при совпадении. Вместо копирования колонок переиспользуем
ref-counting `TRowSet`:

```cpp
// RowId = packed (batchIdx:i32 << 32) | rowIdx:i32
using TRowId = int64_t;

class TRowStore {
public:
    // Retain(rs); возвращает batchIdx для построения TRowId.
    int32_t PushBatch(const TRowSet& rs);
    ~TRowStore(); // Release всех сохранённых батчей

    // Читает значение колонки colIdx строки rowIdx батча batchIdx
    // (используется при сборке выходного TRowSet).
    const TColumn& Column(int32_t batchIdx, int32_t colIdx) const;
    int32_t RowIndex(TRowId id) const;
    int32_t BatchIndex(TRowId id) const;

private:
    std::vector<TRowSet> Batches_; // Retain'd
};

inline TRowId MakeRowId(int32_t batchIdx, int32_t rowIdx) {
    return (static_cast<int64_t>(batchIdx) << 32) | static_cast<uint32_t>(rowIdx);
}
```

### 5. Join hash table + symmetric-probe ядро — `qdb/kernel/join/*.oz`, standalone тест

Разрабатывается и тестируется отдельно от pipeline (как Aggregation п.4),
запускается отдельным `test/test_join_kernel.cpp`.

**Layout одной стороны** — `<ref JoinTable>` (зеркалит `<ref HashTable>`
агрегации, регистрируется в `qdb/modules/qumirdb.cpp::ExternalTypes_`,
`kJoinTableSize` в `compiler.h`):
- sparse: `Keys: <ptr u8>`, `Dist: <ptr i64>`, `SlotIds: <ptr i64>`,
  `Capacity: i64` — **переиспользуются** `rh_lookup_slot`/`rh_insert_displace`
  без изменений.
- dense: `GroupKeys: <ptr u8>` (тип `Key`, Stage 1 = `i64`), `RowBuckets: <ptr
  u8>` (массив `RowBucket`, **новый** dense payload вместо `AggBuffers`),
  `Size: i64`.

**Новый dense payload** — `qdb/kernel/join/join_row_bucket.oz`:
```text
;; на каждый dense slot — заголовок growable массива RowId
(struct RowBucket
  (Count    i64)
  (Capacity i64)
  (Data     <ptr i64>))   ;; heap, qdb_alloc/qdb_realloc — переиспользуются из Aggregation

;; jb_append(bucket_ptr, row_id) — амортизированно O(1), грow x2 от Capacity=4
(fun jb_append ((var bucket <ptr RowBucket>) (var row_id i64)) -> void ...)
```
Сам `RowBucket` (24 байта: 3×i64) — **фиксированной ширины**, поэтому dense
массив `RowBuckets[capacity]` растёт/rehash'ится той же generic-grow логикой,
что `AggBuffers` у Aggregation (просто другой fixed stride). Переменного
размера — только `RowBucket.Data` (отдельная heap-аллокация), которая при
grow/rehash таблицы **не копируется повторно** — переносится указателем.

**Output pairs buffer** — `qdb/kernel/join/join_pair_buffer.oz`, тот же
growable-паттерн:
```text
(struct PairBuffer (Count i64) (Capacity i64) (Data <ptr i64>)) ;; 2×i64 per pair: (leftRowId, rightRowId)
(fun pb_push ((var buf <ptr PairBuffer>) (var left_id i64) (var right_id i64)) -> void ...)
```

**Symmetric probe+insert над батчем** — `qdb/kernel/join/join_update.oz`:
```text
;; own/opp — JoinTable для своей/противоположной стороны.
;; batch — TRowSet текущего батча; key_col_idx — индекс колонки-ключа.
;; batch_idx — индекс батча в TRowStore своей стороны (для RowId).
;; is_left — true если own == left side (определяет порядок (left,right) в PairBuffer).
(fun jt_process_batch (
       (var own  <ref JoinTable>) (var opp <ref JoinTable>)
       (var batch <ref TRowSet>) (var key_col_idx i64)
       (var batch_idx i64) (var is_left bool)
       (var pairs <ref PairBuffer>)) -> void
  ;; for row_idx in [0, batch.RowCount):
  ;;   key = batch.Columns[key_col_idx].Data[row_idx]   ;; i64, no nulls Stage1
  ;;   own_row_id = pack(batch_idx, row_idx)
  ;;   opp_slot = rh_lookup_slot(opp.Keys/.Dist/.SlotIds, opp.Capacity, key)
  ;;   if opp_slot != -1:
  ;;     bucket = opp.RowBuckets[opp_slot]
  ;;     for i in [0, bucket.Count):
  ;;       if is_left: pb_push(pairs, own_row_id, bucket.Data[i])
  ;;       else:       pb_push(pairs, bucket.Data[i], own_row_id)
  ;;   own_slot = rh_lookup_slot(own.Keys/.Dist/.SlotIds, own.Capacity, key)
  ;;   if own_slot == -1:
  ;;     own_slot = own.Size; own.Size += 1; (grow/rehash own table если нужно)
  ;;     rh_insert_displace(own.Keys/.Dist/.SlotIds, own.Capacity, key, own_slot)
  ;;     own.GroupKeys[own_slot] = key; own.RowBuckets[own_slot] = {0,0,null}
  ;;   jb_append(&own.RowBuckets[own_slot], own_row_id)
  ...)
```
`rh_hash`/`rh_key_equal` для `Key=i64` — переиспользуются из
`aggregation/key_ops_i64.oz` без изменений (`ParseFunctionLibrary`/
`MergeKernelLibrary` собирают AST из файлов обеих директорий).

`jt_init(table, capacity)`/`jt_destroy(table)` — аналог `aht_init`/
`aht_destroy` (width-agnostic byte allocation), плюс освобождение всех
`RowBucket.Data`.

### 6. `CompileJoin` — `qdb/kernel/compiler.{h,cpp}`

```cpp
// op==0: init(leftTable/rightTable, capacity=arg)
// op==1: process batch from left  (jt_process_batch(left, right, batch, ..., is_left=true, pairs))
// op==2: process batch from right (is_left=false)
// op==3: drain pairs — копирует накопленные (leftRowId,rightRowId) в выходной буфер, сбрасывает PairBuffer
// op==4 (Stage 3): final scan — для outer join
// otherwise: destroy
using TJoinDispatch = std::function<int64_t(void* leftTable, void* rightTable, void* pairs, TRowSet* batch, int64_t arg, int64_t op)>;

struct TJoinKernels {
    TJoinDispatch Dispatch;
    size_t LeftKeyColIdx, RightKeyColIdx;
};

TJoinKernels CompileJoin(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::string& leftKey, const std::string& rightKey,
    EJoinType type /* Stage1: must be Inner */);
```
Ограничения Stage 1 (зеркалят `CompileAggregate`'s doc-комментарий): `leftKey`/
`rightKey` — единственная `i64`-колонка каждой стороны; `type == Inner`.

### 7. `TRuntimeJoin` — `qdb/exec/join_exec.{h,cpp}`

```cpp
class TRuntimeJoin : public IRuntimeNode {
public:
    TRuntimeJoin(std::unique_ptr<IRuntimeNode> left, std::unique_ptr<IRuntimeNode> right,
        NQumir::NAst::TTypePtr outputType, TJoinKernels kernels);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Left_, Right_;
    TRowStore LeftRows_, RightRows_;
    NQumir::NAst::TTypePtr OutputType_;
    TJoinKernels Kernels_;
    bool LeftDone_ = false, RightDone_ = false;
    // owned JoinTable buffers (kJoinTableSize each) + PairBuffer
};
```

`Next()` — pull-loop, прямой аналог `test_join.cpp`'s
`progress(left)||progress(right)`:
```text
while PairBuffer пуст:
  if !LeftDone_ and Left_->Next(batch):
      batchIdx = LeftRows_.PushBatch(batch)
      Dispatch(op=1, batch, batchIdx)   // jt_process_batch, is_left=true
  elif !RightDone_ and Right_->Next(batch):
      analogично, op=2, is_left=false
  else:
      LeftDone_ = RightDone_ = true   // оба исчерпаны
      // Stage1 (INNER): return false (нет финального скана)
      // Stage3 (OUTER): Dispatch(op=4, ...) — final scan, дозаполняет PairBuffer
      if PairBuffer пуст: return false
Dispatch(op=3, ...) -> массив (leftRowId,rightRowId)
// сборка выходного TRowSet: для каждой пары —
//   left columns  = LeftRows_.Column(batchIdx(leftId), colIdx)[rowIdx(leftId)]
//   right columns = RightRows_.Column(batchIdx(rightId), colIdx)[rowIdx(rightId)]
// аллоцируется новый TRowSet с собственным буфером (как TProjectedRowSetData
// в project_exec.cpp — кастомный Destroy/Private), копированием значений по
// RowId-списку (gather).
```

### 8. Planner / CLI / CMake / тесты

- `exec/planner.cpp::Build`: ветка `TMaybeOp<TJoinOperator>` — **два**
  рекурсивных `Build()` (Left/Right), `CompileJoin`, `ComputeJoinOutputType`.
- `exec/planner.cpp::PrintRuntimePlan`: ветка join — печатает оба поддерева с
  отступом (аналог двух детей).
- `test/CMakeLists.txt`: регистрация `test_join_kernel` (standalone .oz,
  Stage 1 п.5) и `test_join_exec` (e2e через sexp/planner/`TRuntimeJoin`,
  Stage 1 п.8) — по образцу `ut(test_aggregation ...)`.
- e2e-тест переиспользует `GenerateTable`/`NestedLoopInnerJoin` из
  `test/test_join.cpp` как генератор и oracle, но строит входные `TRowSet` (а
  не `std::map`) и гоняет через реальный `TRuntimeJoin`.

---

## Stage 2 — намётки (LEFT / RIGHT / FULL OUTER, deferred final scan)

- `EJoinType` += `Left, Right, Full`. `ComputeJoinOutputType`: для `Left` —
  колонки правой стороны становятся nullable (и наоборот для `Right`; обе для
  `Full`).
- `CompileJoin` op==4 ("final scan"): для `Left`/`Full` — проход по
  `left.GroupKeys[0..left.Size)`; для каждого `i`, если
  `rh_lookup_slot(right, left.GroupKeys[i]) == -1`, для каждого `rowId` в
  `left.RowBuckets[i]` — `pb_push(pairs, rowId, NULL_ROW_ID)`. Симметрично для
  `Right` со сканом `right`. `Full` = оба скана.
- `NULL_ROW_ID = -1` (sentinel) — при сборке выходного `TRowSet` колонки
  противоположной стороны заполняются NULL (mask bit = 0), данные не читаются.
- Корректность: см. доказательство в Context — `rh_lookup_slot == -1` ⇔ "эта
  сторона никогда не видела такой ключ", что исключает повторную эмиссию уже
  сматченных в основном потоковом цикле пар.
- e2e-тесты: расширить `GenerateTable`/`NestedLoopInnerJoin`-аналог в
  `test/test_join.cpp` уже умеет `EmptySides` (граничный случай "всё
  непарное" — хорошая база для LEFT/RIGHT oracle).

## Stage 4 — намётки (generic equi-key: МАКСИМАЛЬНОЕ переиспользование Aggregation)

> **Принцип:** generic-ключ в join не пишется заново — он **переиспользует
> ключевой слой Aggregation целиком**. i64-only в Stage 1 — это лишь ручной
> `jt_process_batch` + проверка в `CompileJoin`; сама хэш-мапа уже generic.

Aggregation уже поддерживает любые int (вкл. **int32**), f64, string и
**композитные** ключи. Это разблокирует реальный TPC-H (ключи `*_key` — int32).

**Карта переиспользования (что НЕ дублируем):**

| Компонент Aggregation | Роль | В join |
|---|---|---|
| `aht_init`/`aht_rehash`/`aht_destroy` | lifecycle хэш-мапы (width-agnostic, `KeySize` в `HashTable`) | **уже** переиспользовано (Stage 1) |
| `rh_lookup_slot`/`rh_insert_displace`/`rh_rehash_into` | generic probe над `<named Key>` | **уже** переиспользовано |
| `BuildAggregateKeyDescriptor` / `RepresentKeyType` (`aggregate_key.cpp`) | раскладка `Key` (Size/Alignment), скаляр/структ/строка | обобщить → `BuildJoinKeyDescriptor` (две схемы) |
| генерация overload'ов `rh_hash(Key)`/`rh_key_equal(Key,Key)` | хэш/равенство под конкретный `Key` | переиспользовать как есть |
| кодоген чтения ключа из колонок (`gen.cpp`/`BuildColumnValueAst`) | read + assemble `Key` (композит/строка/null) | переиспользовать для генерации `jt_process_batch`-prelude |

**Что собственно делаем (только «ключевой» слой; бакеты/gather/батчинг/RowStore/
планировщик не меняются):**
- `BuildJoinKeyDescriptor` — почти `BuildAggregateKeyDescriptor`, но над **двумя**
  схемами (left/right). Единственная join-специфика: имена/позиции колонок
  разные, а **представление `Key` обязано совпадать** (иначе left- и right-ключ
  не столкнутся в одной хэш-таблице) — проверяется по `RepresentKeyType`.
- **Генерировать** `jt_process_batch` per-query (как Aggregation генерирует свой
  dispatch в `gen.cpp`): prelude читает ключевые колонки и собирает `Key`
  (переиспользуя тот же column-read кодоген), тело зовёт уже-generic
  `rh_lookup_slot`/`rh_insert_displace`<Key> + сгенерированные `rh_hash`/
  `rh_key_equal`<Key>. Ручной i64-`jt_process_batch` работал лишь потому, что
  чтение i64 тривиально.
- `CompileJoin`: убрать i64-проверку, передавать вычисленный `KeySize`,
  домержить сгенерированные overload'ы (тот же `AllowOverloads`-контракт).

Идеал: Join и Aggregation делят один набор `key_ops_*.oz` + `<named Key>`-
инстанциацию + descriptor/codegen; расходятся только в dense-payload (RowId-
бакеты vs reducers) — а он уже к типу ключа безразличен.

### Stage 4 — реализация (K1–K4)

**Развилка по объёму (согласовано): fixed-keys первым, строки — следом.**
- **Fixed** (int любой ширины — вкл. **int32**, f64, композит-fixed): переиспользует
  простые generic `rh_lookup_slot`/`rh_insert_displace`/`aht_rehash` (у join уже
  есть, нужно лишь `i64`→`<named Key>`). **Разблокирует реальный TPC-H** (`*_key`
  — int32). Композитные fixed-ключи (напр. `(i32,i32)`) идут сразу.
- **String** (нужен dual-key: `StringView`↔`OwnedString` через `rh_lookup_dual`/
  `aht_upsert_dual`/`aht_rehash_dual`) — отдельным K-шагом следом, тоже на reuse.

Декомпозиция:
- **K1** — `BuildJoinKeyDescriptor` (`join_key.{h,cpp}`): единый column-name-
  независимый `Key` для обеих сторон (имя по типам) + `Left/RightColumnIndex` на
  поле + проверка совместимости. **Переиспользует `RepresentKeyType`** (вынесен в
  публичный API `aggregate_key.h` — поведение агрегации не тронуто). Тесты (8):
  i32/i64/f64/string/композит, reject несовместимых/отсутствующих. **[x] готово.**
- **K2 [x]** — `.oz` ядро `jt_emit_and_insert(own, opp, key:<named Key>, …)` (generic,
  моё i64-тело лифтнуто на `<named Key>`; i64 `jt_process_batch` стал тонкой
  обёрткой над ним — дедуп) + генерируемый prelude `jt_process_left/right`
  (`join_gen.cpp`: read+assemble `Key` через `BuildColumnValueAst`+
  `TStructConstructExpr`, по `Left/RightColumnIndex`; `GenJoinKeyOpsFunDecls`
  reuse `GenKeyOperationFunDecls`; `GenJoinKeyTypeDecls` для `(type ...)`).
  Standalone-тест на int32 vs nested-loop.
- **K3 [x]** — `CompileJoin` без i64-проверки: `BuildJoinKeyDescriptor` → `KeySize`,
  домерж type-decls + key-ops + generated `jt_process_left/right`. `TJoinKernels`
  теперь `ProcessLeft`/`ProcessRight` (без keyColIdx); `TRuntimeJoin`/planner
  обновлены, planner принимает список ключей (композит). Тесты обновлены.
- **K4 [x]** — e2e + CLI на **реальном TPC-H**: `orders ⋈ customer` по int32
  `custkey` через `build/join_tpch.sh` → **1.5M строк**. Planner-тесты на int32
  и композит `(i32,i32)`.

> Реализация K1: `RepresentKeyType`/`AlignUp`/`TRepresentedKeyType` вынесены в
> `aggregate_key.h` (из анонимного namespace), 21/21 агрегации зелёные.
> `TJoinKeyDescriptor`/`TJoinKeyField` + `BuildJoinKeyDescriptor` в
> `join_key.{h,cpp}`; `TypeName` по типам (не именам колонок) → обе стороны
> инстанцируют один `Key`. 15/15 ctest.
>
> Грабли по дороге: обновлённый Qumir-сабмодуль сделал `TNumberExpr::IsFloat`
> методом → поправил `IsFloat()` в `compiler.cpp` (Qumir-фиксы NaN-семантики и
> bool short-circuit подтянулись пересборкой).
>
> **Итог Stage 4 (fixed-keys): int/int32/f64/композит-fixed работают end-to-end,
> реальный TPC-H поехал.** Строки (dual-key) — отдельным шагом (см. развилку).

## Stage 4 — намётки (Grace partitioning как аддитивная обёртка)

- Партиционирование `hash(key) % N` — НЕ меняет ядро из Stage 1-3. Каждая
  партиция = независимый экземпляр `(LeftTable_p, RightTable_p, RowStore_p,
  PairBuffer_p)`.
- Router: перед `jt_process_batch` строка маршрутизируется в партицию `p =
  hash(key) % N` (тот же `rh_hash`, что внутри таблицы — co-location
  гарантируется автоматически).
- `IsTablesSwapRequired`-правило (выведенное из анализа YDB) применимо НЕ к
  выбору build/probe (симметричная схема не строит/не перестраивает — обе
  таблицы растут инкрементально), а к **spill-приоритету**: партиция, чья
  суммарная `RowStore`-память превышает лимит, спиллится на диск целиком
  (`LeftTable_p`+`RightTable_p`+`RowStore_p` как единая единица), стриминг
  продолжается для остальных партиций.
- При spill партиции `p`: входящие строки для `p` дописываются в
  on-disk append log; при "разгрузке" партиция читается целиком и
  прогоняется через тот же `jt_process_batch`-кернел в режиме "оба входа —
  весь лог" (без потоковости — bounded by partition size after rebalancing).
- Bloom-фильтр (как в YDB) — опциональная оптимизация раннего отбрасывания,
  не входит в Stage 1-4 минимального плана; добавляется как доп. поле
  `JoinTable.Bloom: <ptr u8>` без изменения ABI вызовов.
- Строки/large values: RowStore хранит `TRowSet` целиком (включая
  variable-length колонки через `Offsets`) — Stage 1-4 уже это поддерживают
  "бесплатно" через `Retain`/`Release`; отдельной работы для строк в самом
  Row Store не требуется. Ограничение Stage 1-3 — только в типе **ключа**
  (`i64`), не значений.

## Stage 5 — намётки (MARK / SINGLE, in-kernel residual)

- **MARK join** (флэттенинг коррелированных подзапросов с `OR`/`IN`/`EXISTS`):
  вариант SEMI, который не фильтрует, а добавляет **булеву колонку-метку**
  (`mark = ∃ совпадение`) к строкам сохраняемой стороны. Та же emit-логика, что
  SEMI, но эмитятся **все** left-строки с проставленным `mark` (включая
  непросмаченные → `mark=false`); требует обработки `mark=UNKNOWN` для null-ключей.
- **SINGLE / scalar join** (скалярный подзапрос, «не более одной строки
  справа»): INNER с ассертом кардинальности `≤1` на ключ (иначе runtime-ошибка
  «scalar subquery returned more than one row»). Переиспользует inner-ядро +
  проверку `left.RowBuckets[k].Count == 0` перед вставкой / `bucket.Count ≤ 1`.
- **In-kernel residual `θ(left,right)`** (отложено — см. "Локус фильтра"):
  компилируемый предикат над парой схем `left⊕right`, вызывается в
  `jt_process_batch` сразу после того, как `rh_lookup_slot` нашёл бакет, перед
  `pb_push`/инкрементом «существования». ABI — расширение `CompileJoin`
  (доп. compiled-предикат, аналог `CompileFilter`, но над двумя входами).
  **Обязателен** для корректных OUTER/SEMI/ANTI/MARK с двусторонним `θ`; для
  INNER — perf-оптимизация. В текущий план не входит — добавляется отдельным
  согласованием.

---

## Детальный порядок реализации

### A. `TJoinOperator` + `MakeJoin` (Stage 1 §1)
- [x] `qdb/ops/join.{h,cpp}`: `TJoinOperator`, `EJoinType`,
  `ComputeJoinOutputType` (ошибка при пересечении имён колонок), `MakeJoin`.
- [x] `qdb/CMakeLists.txt`: добавить `join.cpp` в таргет `qumirdb`.

> Реализация: узел сделан сразу богаче исходного Stage-1-наброска (по запросу —
> «узел должен иметь левый/правый входы, тип join, список ключей, фильтр»):
> - `EJoinType` — полный набор `{Inner, Left, Right, Full, LeftSemi, RightSemi,
>   LeftAnti, RightAnti}` (не только `Inner`). Кодогенерация ядра пока покроет
>   лишь `Inner` (Stage 1), но логический узел/вывод схемы уже знают все.
> - **Список ключей** `std::vector<TJoinKey>` (`TJoinKey{Left,Right}` —
>   пара имён колонок), а не одиночный `leftKey`/`rightKey`.
> - **Поле фильтра** `Filter_` (`TExprPtr`, nullable) — residual θ над
>   склеенной строкой `left⊕right`, семантика «применить ДО emit». Хранится
>   распарсенным/неаннотированным (как `TFilterOperator::Predicate_`).
>   `MakeJoin(..., const std::string& filter = "")` парсит его как Filter.
> - Аксессор типа назван `JoinType()` (не `Type()` — иначе перекрывает поле
>   `TExpr::Type`).
> - `ComputeJoinOutputType` → `std::expected<TTypePtr, TError>`: Inner = конкат
>   обеих сторон; Left/Right/Full оборачивают NULL-дополняемую сторону в
>   `TNullable`; Semi/Anti оставляют только сохраняемую сторону. Ошибка при
>   пересечении имён колонок (без алиасинга — Stage 1). `ComputeReferencedColumns`
>   = ключи обеих сторон ∪ unbound-vars фильтра.
> - `ToString` печатает `(rel join L R (on a c)... (type inner) [(filter ...)])`.
> - Тесты: `test/test_join_node.cpp` — 10 кейсов (схемы Inner/L/R/Full/Semi/Anti,
>   reject пересечения имён и пустых ключей, парсинг фильтра + referenced cols,
>   roundtrip `JoinTypeName`/`ParseJoinType`). Зелёные.
> - Эталон: фильтр заведён в `test/test_join.cpp` (`InnerJoin`/
>   `NestedLoopInnerJoin` + тест `ResidualFilterAppliedBeforeEmit`) — точка
>   вставки фильтра = момент матча в probe-шаге, до `emit`. **Важно:** этот
>   оракул построчный; боевой движок колоночный — в ядре residual считается над
>   парой gather-нутых по `RowId` строк, а не над `std::map`.
>
> Дальше (не сделано): sexp parser/printer для `(rel join ...)` (шаг B),
> multi-input pipeline/typing/pruning (шаг C).

### B. Sexp printer/parser (Stage 1 §2)
- [x] `qdb/sexp/printer.cpp`: `PrintRel` ветка `TJoinOperator::OpId`.
- [x] `qdb/sexp/parser.cpp`: `MakeRelParsers` ветка `(rel join <left> <right>
  (on l r) (type inner))`.
- [x] Roundtrip-тест (печать → парсинг → совпадение).

> Реализация: канонический sexp (лаконичная форма) —
> `(rel join <left> <right> ((lk rk) (lk rk)...) (<type>) [(<pred>)])`:
> ключи единым списком пар, тип — голым словом в скобках, фильтр — прямо
> предикатом без метки `filter`. Пример:
> `(rel join (rel source ...) (rel source ...) ((a c) (b d)) (left) (< b d))`.
> - Printer (`printer.cpp`): два дочерних через `PrintExpr`, затем список
>   `((lk rk)...)`, всегда `(<type>)` (детерминизм roundtrip), и предикат
>   напрямую `PrintExpr` — только если фильтр задан.
> - Parser (`parser.cpp`): `co_await h.Expr()` для left/right; `Take('(')` +
>   цикл пар `(lk rk)` до `)`; `Take('(')` + слово типа (`ParseJoinType`,
>   ошибка на неизвестное) + `Take(')')`; затем опциональный предикат
>   (`h.Next()`: `)` ⇒ нет фильтра, иначе `Unget`+`h.Expr()`). Пустой список
>   ключей → `TError`. До конструирования зовётся `ComputeJoinOutputType` —
>   пересечение имён колонок всплывает уже на парсинге. Узел строится напрямую
>   (`make_shared<TJoinOperator>`), как остальные rel-парсеры. `ToString`
>   приведён к той же форме.
> - Тесты (`test/test_sexp.cpp`): `SexpPrinter.Join`,
>   `SexpPrinter.JoinWithFilterAndMultipleKeys`, `SexpParser.JoinPrintRoundtrip`
>   (`(left)` + `(< b d)`), `SexpParser.JoinRejectsMissingKeys` (`()` ключи).
>   Зелёные (14/14 в test_sexp, 10/10 полного ctest).

### C. Pipeline: multi-input обобщение + typing/pruning для Join (Stage 1 §3 + "Открытый вопрос")
- [x] `qdb/ops/operator.h`: `IOperator::RequiredColumnsForChild(size_t,
  const std::unordered_set<std::string>&)` с дефолтной реализацией (текущая
  формула, для single-input операторов поведение не меняется).
- [x] `pipeline/column_pruning.cpp`: `walk` обобщён на N children через
  `RequiredColumnsForChild`.
- [x] `pipeline/typing.cpp::AnnotateTypes`: ветка `TJoinOperator`.
- [x] `TJoinOperator::RequiredColumnsForChild`: join-ключ своей стороны ∪
  (parentRequired ∩ ownColumns).
- [x] Регресс: существующие тесты (`test_aggregate` гоняет переписанный
  `walk` на project/aggregate) проходят без изменений.

> Реализация:
> - `IOperator::RequiredColumnsForChild(childIdx, needed)` — виртуальный, дефолт
>   = `needed ∪ ComputeReferencedColumns()` (pass-through: filter/source). Это
>   вынесло прежнюю inline-логику column_pruning в полиморфизм.
> - `TProjectOperator`/`TAggregateOperator` переопределяют → `return
>   ComputeReferencedColumns()` (schema-defining, игнорируют `needed`) — ровно
>   прежнее поведение их inline-веток.
> - `TJoinOperator::RequiredColumnsForChild` (`join.cpp`): для стороны `i` —
>   ключ своей стороны ∪ (vars фильтра ∩ колонки стороны) ∪ (`needed` ∩ колонки
>   стороны). `childIdx 0 = left, 1 = right`.
> - `column_pruning.cpp::walk` переписан: source — спец-случай (нарезка
>   `ReturnType`+`ParamTypes[0]`), иначе цикл `for i in Children()` —
>   `required = op->RequiredColumnsForChild(i, needed)`, нарезка `ParamTypes[i]`,
>   рекурсия. Хелпер `narrowStruct`. Single-input (i=0) — байт-в-байт прежнее.
> - `typing.cpp`: ветка `TJoinOperator` — `ParamTypes = [left.Return,
>   right.Return]`, `ReturnType = ComputeJoinOutputType(...)` (бот-ап рекурсия
>   уже обходит оба ребёнка через `Children()`).
> - **Замечание:** pruning нарезает входы (`ParamTypes[i]`) и схемы источников,
>   но НЕ пересчитывает `join.ReturnType` (как и aggregate) — фактический
>   выходной тип соберёт планировщик (шаг F) из суженных входов.
> - Тесты `test/test_join_pipeline.cpp` (3 кейса): typing (2 ParamTypes +
>   конкатенация выхода), независимая нарезка сторон при project сверху, и что
>   ключи остаются даже когда не выбраны downstream. Зелёные (11/11 ctest).

### Дизайн: flush выходных строк + батчевание (D2/D3/G)

**Шов C++ ↔ .oz.** Ядро (.oz) делает только matching: `jt_process_batch`
накапливает пары `(leftRowId, rightRowId)` в `PairBuffer`; op `drain` сливает
их в C++-массив. **Батчевание и gather — целиком на C++.** Ядро = «кто с кем
совпал», C++ = «как нарезать и собрать строки». Этот шов позволяет собрать и
протестировать весь C++-плумбинг (D1–D3) на **стаб-матчере** (чистый C++
hash-join, заполняющий список пар) ДО появления JIT-ядра (E–F); в G стаб
заменяется реальным `Dispatch`.

**Aggregate vs Join.** Aggregate эмитит весь результат за один `Next()` (групп
мало). Join так нельзя — выход бывает огромным (кросс горячего ключа), поэтому
выход **дренажится порциями** `kJoinOutputBatchRows` (напр. 1024).

**PairBuffer как очередь с курсором** (`TRuntimeJoin`):
`Pairs: vector<(TRowId,TRowId)>` растёт при обработке входа; `Cursor` — первая
невыданная пара. `Next(out)` — дренаж-сначала, добор по необходимости:
```
loop:
  if Cursor < Pairs.size():                       // отдать готовое порциями
      n = min(kJoinOutputBatchRows, size - Cursor)
      out = GatherBatch(Pairs[Cursor .. Cursor+n]) // flush (gather по RowId)
      Cursor += n; return true
  Pairs.clear(); Cursor = 0                        // буфер пуст → сброс
  if BothExhausted: return false
  if !PullOneInputBatch():                         // добрать вход (left/right поочерёдно)
      BothExhausted = true; RunFinalScan()         // Stage 3 OUTER; INNER — no-op
```
Память ограничена парами от одного входного батча (Stage 1; spill — Stage 5).

**`GatherBatch` (flush).** Колоночный take по `RowId` из двух `TRowStore`, по
образцу owned-buffer aggregate (вектор на колонку + `TColumn` поверх + custom
`Destroy`): left-колонки берутся из `LeftRows_` по `leftRowId`, right — из
`RightRows_` по `rightRowId`; fixed-width — копия значения + бит валидности;
string — два прохода (длины→offsets+байты); `NULL_ROW_ID` (паддинг OUTER) → бит
null. Опирается на примитив **take колонки по списку индексов** (D2).

### D1. Row Store (Stage 1 §4)
- [x] `qdb/exec/join_exec.h`: `TRowStore`, `TRowId`,
  `MakeRowId`/`BatchIndex`/`RowIndex` — чистый C++, `Retain`/`Release` из
  `io/io.h`. Удерживает все батчи стороны до уничтожения узла.
- [x] Unit-тест `TRowStore`: push нескольких батчей, чтение колонок по `RowId`,
  корректный `Release` в деструкторе (через `RefCount`).

> Реализация: `join_exec.h` (header-only). `TRowId = (batchIdx<<32)|rowIdx`,
> `kNullRowId = -1`, `MakeRowId`/`BatchIndex`/`RowIndex` (rowIdx через `uint32`
> — переживает значения близкие к 2^31). `TRowStore` — `std::vector<TRowSet>`,
> `PushBatch(TRowSet)` забирает владение (move-семантика: `RefCount` живёт ВНУТРИ
> структуры, поэтому ровно одна каноническая копия — в сторе; вызывающий после
> push батч не `Release`-ит), деструктор `Release`-ит все. `Column(batchIdx|
> RowId, colIdx)`. Non-copyable. Тесты `test/test_join_exec.cpp` (3): pack
> round-trip RowId, чтение по batch/RowId, `Destroy` ровно один раз на батч при
> разрушении стора. 12/12 ctest.

### D2. Column take/gather по RowId (Stage 1 §7, примитив flush)
- [x] `qdb/exec/join_exec.{h,cpp}`: `TakeColumn(store, rowIds, srcColIdx,
  type, out)` — fixed-width + string + null mask; `kNullRowId` → null-бит.
  Owned-буферы (`TGatheredColumn`) с тем же владением, что у aggregate.
- [x] Unit-тест: вручную собранный `TRowStore` + список `RowId` (вкл.
  `kNullRowId`) → ожидаемые значения/маска для i64 и string колонок.

> Реализация: `TGatheredColumn{Data, Offsets, Mask, Column}` — owned-буферы,
> `Column` смотрит в них. `JoinColumnFixedWidth(type)` → байтовая ширина (int по
> `BitWidth()/8`, float=8) либо 0 для string (bool/прочее — `TError`, Stage 1).
> `TakeColumn`: fixed — копия `width` байт/строку, null/`kNullRowId` → бит маски
> 0 + нули в данных; string — два прохода (длины→offsets, затем payload),
> выход `OffsetWidth=8`. Source-null уважается через `SourceValid` (с
> `MaskBitOffset`); source-offsets читаются `OffsetAt` (ширина 4/8). Маска
> опускается (`nullptr`), если null не было. Тесты `test/test_join_exec.cpp` (4):
> i64+null-паддинг, all-valid→без маски, source-null, string-gather (вкл. пустую
> строку и null). 12/12 ctest.

### D3. Output assembly + батчевание (Stage 1 §7, стаб-матчер)
- [x] `qdb/exec/join_exec.{h,cpp}`: `TJoinColumnRef{Side, SrcColIdx, Type}`
  (порядок = `ComputeJoinOutputType`); `TJoinOutputBuilder::NextBatch` собирает
  `TRowSet` из среза пар через `TakeColumn`; custom `Destroy`.
- [x] Дренаж порциями `kJoinOutputBatchRows` (курсор внутри builder).
- [x] Unit-тест со **стаб-матчером** (nested-loop по ключам заполняет пары):
  сверка с oracle на уровне `TRowSet`, проверка границ батча. Без `.oz`/JIT.

> Реализация: `EJoinSide`, `TJoinColumnRef`, `kJoinOutputBatchRows=1024`.
> `TJoinOutputBuilder(left*, right*, columns, batchRows)` хранит два параллельных
> вектора `LeftIds_/RightIds_` + `Cursor_`; `AddPair` (матчер/стаб), `NextBatch`
> берёт срез ≤ batchRows, для каждой колонки `TakeColumn` из нужной стороны →
> owned `TJoinedRowSetData{Gathered, Columns}` + `DestroyJoinedRowSet`.
> `ClearPairs`/`HasPending`/`PendingCount` для дренаж-цикла G. Тесты (2):
> стаб-матчер vs nested-loop oracle (4 матча = 2×2 батча при batchRows=2),
> пустые пары → нет батча. 12/12 ctest.
>
> **Шов для G:** input→pairs (ядро/стаб через `AddPair`) отделён от pairs→batches
> (`NextBatch`). В G стаб-матчер заменится на `Dispatch(process)`+`Dispatch(drain)`,
> а `NextBatch`/gather переиспользуются как есть.

### E1. Dense-структуры ядра (.oz), standalone (Stage 1 §5)
- [x] `<ref JoinTable>` — layout + регистрация в
  `qdb/modules/qumirdb.cpp::ExternalTypes_`, `kJoinTableSize` в `compiler.h`.
  `jt_init`/`jt_destroy`.
- [x] Dense RowId-bucket рост `jb_append` (qdb_alloc/realloc, x2 от 4).
- [x] `PairBuffer` (внешний тип) + `pb_push`/`pb_destroy`.
- [x] Standalone-тесты (`test/test_join_kernel.cpp`).

> Реализация (итоговая, после дедуп-пересмотра): **join переиспользует
> `HashTable` агрегации как свою хэш-мапу** — отдельного `JoinTable`-типа НЕТ.
> Dense per-slot RowId-бакет хранится в generic `AggBuffers` тремя int64-«колонками»
> (`NumAggs=3`): `AggBuffers[0]`=длина, `[1]`=ёмкость, `[2]`=heap-указатель данных
> (как i64). Это даёт бесплатно: `aht_init` зануляет (пустые бакеты), `aht_rehash`
> копирует int64 → **перенос указателей бакетов без рекопирования**,
> `aht_init`/`aht_rehash`/`aht_destroy` не дублируются.
> - В `qumirdb.cpp` остаётся только новый внешний тип `PairBuffer` (24 б:
>   Count/Capacity/Data). `kPairBufferSize=24` в `compiler.h`; join использует
>   `kHashTableSize`.
> - `qdb/kernel/join/join_table.oz`: `jt_init` (обёртка → `aht_init(ht, cap, 3,
>   key_size)`), `jt_destroy` (освобождает per-slot блоки `AggBuffers[2][0..Size)`,
>   затем `aht_destroy`), `jb_append` (на `AggBuffers`, рост 4→8→16, указатель
>   через i64), `pb_push`/`pb_destroy`.
> - `ReadJoinKernel(name)` в `lib.{h,cpp}` (mirror `ReadAggregationKernel`).
> - Две мелочи-цены: указатель-как-i64 в `AggBuffers[2]` (документировано);
>   per-slot free в `jt_destroy` (aht_destroy про них не знает).
> - Тесты: layout `PairBuffer` (+ что `JoinTable`-типа НЕТ); `jt_init`/`jt_destroy`
>   (NumAggs=3, поля); `jb_append`; `pb_push`. С первой компиляции.

### E2. `jt_process_batch` symmetric probe+insert (.oz), standalone (Stage 1 §5)
- [x] `qdb/kernel/join/join_update.oz`: `jt_process_batch` — переиспользует
  `rh_lookup_slot`/`rh_insert_displace`/`aht_rehash` (aggregation) + `key_ops_i64.oz`.
  Накапливает пары в `PairBuffer`. `jt_rehash` НЕ нужен (reuse `aht_rehash`).
- [x] `test/test_join_kernel.cpp`: standalone, слитый `PairBuffer` против
  `NestedLoopInnerJoin` (вкл. кейс с rehash).

> Реализация: `jt_process_batch(own, opp, batch, key_col_idx, batch_idx,
> is_left, pairs)` над `<ref HashTable>` обеих сторон. Цикл по строкам (учёт
> `Selection`): read i64-ключ → `own_row_id = (batch_idx<<32)|row` → probe
> противоположной (`rh_lookup_slot`, дренаж её бакета `AggBuffers[2][slot]` в
> `pb_push`, порядок пары по `is_left`) → вставка в свою (`rh_lookup_slot`; при
> miss — grow-check + `aht_rehash` с i64-witness, `rh_insert_displace`,
> `GroupKeys[slot]=key`, **зануление нового dense-слота** т.к. `aht_rehash` не
> чистит хвост, `jb_append`). `is_left` сделан `i64` (надёжнее на C-границе).
> Сборка библиотеки в тесте: `key_ops_i64` + `robin_hood_rehash_generic` +
> `aggregation_hashtable_generic` (exclude `aht_update`, т.к. нужен
> `agg_apply_reducers`) + `join_table.oz` + `join_update.oz`, `AllowOverloads=true`.
> Грабли: `pb_push` возвращает `bool` — oz требует использовать результат
> (обернул в `(if (! ...) (return #f))`, заодно проброс OOM). Тесты (6): layout,
> init/destroy, jb_append, pb_push, process_batch vs nested-loop, и крупный кейс
> с `capacity=4` (принудительный rehash, проверка переноса бакетов + что каждая
> эмитнутая пара имеет равные ключи). С первой компиляции (после фикса pb_push),
> 13/13 ctest.

### F. `CompileJoin` (Stage 1 §6)
- [x] `qdb/kernel/compiler.{h,cpp}`: `TJoinKernels`, `CompileJoin` — собирает
  библиотеку из `qdb/kernel/join/*.oz` + переиспользуемых `aggregation/*.oz`.
- [x] Валидация ограничений Stage 1 (`i64` key, `Inner`) — `NQumir::TError`.

> Реализация: `BuildJoinKernelLibrary()` вынесен в `lib.{h,cpp}` (дедуп —
> используется и тестом E2, и `CompileJoin`): собирает `key_ops_i64` +
> `robin_hood_rehash_generic` + `aggregation_hashtable_generic` (exclude
> `aht_update`) + `join_table.oz` + `join_update.oz`.
> - `TJoinKernels` (`compiler.h`) — НЕ один opaque dispatch, а типизированные
>   `std::function`: `Init(table, capacity)`, `Process(own, opp, batch, keyColIdx,
>   batchIdx, isLeft, pairs)`, `DestroyTable(table)`, `DestroyPairs(pairs)` +
>   `LeftKeyColIdx`/`RightKeyColIdx`. Так проще для G, чем op-код диспетчер.
> - `CompileJoin` компилирует 4 entry (`jt_init`/`jt_process_batch`/`jt_destroy`/
>   `pb_destroy`), каждый — свой `TLLVMRunner` (как 3 раннера у aggregate),
>   `AllowOverloads=true`, `OptLevel=3`, runner captured в лямбде (держит JIT).
>   Валидация: `type==Inner`, ключи — i64-колонки (через `findKey`, индексы
>   `LeftKeyColIdx`/`RightKeyColIdx`); иначе `TError`. `compiler.h` подключает
>   `ops/join.h` для `EJoinType` (слой уже зависит от `ops/aggregate.h`).
> - Тесты (`test_join_kernel.cpp`, +2): `CompileJoin` end-to-end (init→process×2
>   →drain vs nested-loop, destroy) и reject (string-ключ / не-Inner / нет
>   колонки). 13/13 ctest.

### G. `TRuntimeJoin` (Stage 1 §7) — склейка D1–D3 с реальным ядром
- [x] `qdb/exec/join_exec.{h,cpp}`: `TRuntimeJoin::Next()` — дренаж-цикл из D3,
  `PullOneInputBatch` зовёт `Left_/Right_->Next()` + `Kernels_.Process` (F).
- [x] Дренаж ядрового `PairBuffer` в `TJoinOutputBuilder` (D3); gather — D2/D3.
- [x] e2e-проверка `TRuntimeJoin` против nested-loop (exact + randomized).

> Реализация: `TRuntimeJoin` владеет двумя `TRowStore` (D1), двумя
> `HashTable`-буферами (`kHashTableSize`), `PairBuffer`-состоянием
> (`TPairBufferState`, static_assert == `kPairBufferSize`), `TJoinOutputBuilder`
> (D3, в `std::optional` — строится в теле конструктора, т.к. держит `&LeftRows_/
> &RightRows_`). Колонки выхода вычисляются в конструкторе из
> `Left_/Right_->OutputType()` (все left ++ все right).
> - `Next()`: `EnsureInit` (ленивая `Kernels_.Init` обеих таблиц, cap=256) →
>   цикл `Builder_->NextBatch` (отдать чанк) / `ClearPairs` / `PullOneInputBatch`.
> - `PullOneInputBatch`: «сначала весь left, потом right» (корректно для
>   probe-opposite/insert-own — каждая пара эмитится один раз при обработке
>   второй стороны). `Next()` → `PushBatch` в `TRowStore` (move-владение,
>   batch_idx для RowId) → `Kernels_.Process(own, opp, &store.Batch(bi),
>   keyColIdx, bi, isLeft, &PairBuffer_)` → `DrainKernelPairs` (копирует пары в
>   builder, сбрасывает `PairBuffer_.Count=0` — переиспользование аллокации).
> - Деструктор: `DestroyTable`×2 (если Initialized) + `DestroyPairs` (null-safe).
>   `TRowStore`-dtor'ы Release-ят батчи после.
> - Грабли: `test_join_exec` main без `TLLVMInitializer` → «no targets
>   registered» при JIT; добавил инициализатор.
> - Тесты (`test_join_exec.cpp`, +2): exact 4-строки (полная сверка
>   `(lk,lv,rk,rv)`) и randomized 50×45 строк, разбитых на батчи по 10 (multi-batch
>   RowId, каждая строка имеет равные ключи, итог = nested-loop count). 13/13 ctest.

### H. Planner / CMake / e2e (Stage 1 §8)
- [x] `exec/planner.cpp::Build` + `PrintRuntimePlan`: ветка `TJoinOperator`
  (два рекурсивных `Build()`).
- [x] `test/CMakeLists.txt`: `test_join_kernel`, `test_join_exec`,
  `test_join_node`, `test_join_pipeline`, `test_join_planner`.
- [x] `test_join_planner.cpp`: sexp → typing → pruning → planner → `TRuntimeJoin`,
  сверка с nested-loop на уровне `TRowSet`.
- [x] CLI smoke: реальный прогон `(rel join ...)` на двух parquet (`build/join_inner.sh`).

> Реализация: ветка `TJoinOperator` в `planner.cpp` (`Build` + `PrintRuntimePlan`).
> `Build` использует **физические (pruned) типы** из `input->OutputType()` (как
> filter/aggregate), зовёт `CompileJoin` + `ComputeJoinOutputType` от физ-типов,
> строит `TRuntimeJoin`. Stage-1 guard'ы: `Filter()` → ошибка (residual пока нет),
> `Keys().size()!=1` → ошибка. `PrintRuntimePlan` печатает join с двумя
> поддеревьями.
> - Тесты `test_join_planner.cpp` (2): inner e2e через sexp; project-сверху →
>   pruning сужает входы, результат верен.
> - **CLI smoke**: `build/join_inner.sh` — генерит два i64-parquet (pyarrow) и
>   гоняет `(rel join L R ((lk rk)) (inner))` через `qdb -i`. Выдал корректные 5
>   строк. Логический + runtime план печатаются (join-ветка в обоих принтерах).
> - **Ограничение, всплывшее на TPC-H**: ключи TPC-H (`*_key`) — int32, а Stage 1
>   принимает только i64 (`CompileJoin` корректно отвергает с понятной ошибкой).
>   Реальный join на TPC-H ждёт Stage 4 (generic key, int32/composite). Поэтому
>   smoke на синтетических i64-parquet.
> - 14/14 ctest.

---

## Stage 1 — ГОТОВ ✅

Полный путь INNER equi-join (i64-ключ, in-memory) работает end-to-end:
`(rel join ...)` sexp → parser → typing → column pruning → planner → `CompileJoin`
(JIT symmetric probe+insert поверх переиспользованной `HashTable`) → `TRuntimeJoin`
→ батчированный gather-выход. Проверено юнитами (узел/sexp/pipeline/RowStore/
gather/kernel/CompileJoin/runtime) и e2e (planner + CLI на parquet). 14/14 ctest.

Следующее (по желанию): Stage 2 OUTER (deferred final scan), Stage 3b SEMI/ANTI,
Stage 4 generic key (разблокирует int32/composite ключи → реальный TPC-H),
residual θ в ядре.

---

## Проверка

- Stage 1: `cmake --build build -j` зелёный; `ctest --output-on-failure` —
  `test_join_kernel` и `test_join_exec` проходят; для случайных таблиц
  (несколько seed/размеров/распределений ключей, как в `test_join.cpp`)
  результат `TRuntimeJoin` побайтово совпадает (с точностью до порядка строк)
  с `NestedLoopInnerJoin`-oracle.
- Stage 3 (OUTER): те же случайные таблицы + `EmptySides`-подобные кейсы;
  количество NULL-строк = `|own.Size| - |{k : rh_lookup_slot(opp,k)!=-1}|`.
- Stage 4: при `N>1` партициях результат идентичен `N=1` (партиционирование
  не меняет семантику, только распределение работы/памяти).
