# План: вычисляемые проекции (мутации колонок) — `select <expr> as <name>`

## Context

`select x*2 as y from t where x < 100`:

- `where x < 100` — уже работает. `TFilterOperator`/`CompileFilter`/
  `GenFilterKernelAst` компилируют предикат в векторизованное ядро, которое
  заполняет `rowSet.Selection`. Эта часть плана его не трогает.
- `x*2 as y` — НЕ работает. `TProjectOperator`/`MakeProject`
  (`qdb/ops/project.{h,cpp}`) уже хранят `TProjectionSpec{Name, Expression}`
  с произвольным разобранным `TExprPtr`, sexp printer/parser уже умеют печатать
  и парсить произвольные выражения в `(rel project ...)`, и
  `ComputeReferencedColumns()` уже общий (`FindUnboundVars` по всем
  projection-выражениям, а не только по идентификаторам). Но:
  - `pipeline/typing.cpp` ставит полю выходной схемы тип только если
    `spec.Expression` — голый `TIdentExpr`; для любого вычисляемого выражения
    `fieldType = nullptr`.
  - `exec/planner.cpp::Build` для `TProjectOperator` бросает
    `"project expression kernels are not implemented yet"`, если хоть одна
    projection — не голый идентификатор.

Под "мутацией колонки" здесь понимается **projection, чьё выражение — не
голый identifier**: оно вычисляет новое значение из одной или нескольких
входных колонок (`x*2`, `x+y`, `cast(x,f64)`, `x<100`, в Stage 2 — строковые
операции).

Цель плана — ответить на вопрос "как их делать вообще" и довести
`TRuntimeProject` до исполнения произвольных вычисляемых projection-списков,
смешанных с identity-колонками.

---

## Главный вопрос: "новая колонка" vs "in-place"

Короткий ответ: **на логическом уровне — всегда новая колонка для вычисляемых
выражений; "in-place" — это вопрос физического слоя (переиспользование C++
буферов между батчами одного узла), а не вопрос "переписать чужую колонку".**
Подробно:

### Почему нельзя писать прямо в буфер входной колонки

1. **Source-колонки read-only.** `TParquetSource::Next` (`io/parquet/source.cpp`)
   указывает `TColumn.Data`/`Mask`/`Offsets` прямо в буферы Arrow
   `RecordBatch`, время жизни которых держит `shared_ptr` в `TBatchData`
   (`rowSet.Private`). Эти буферы могут быть переиспользованы/расшарены Arrow
   внутри. Писать в них нельзя в принципе.
2. **Та же колонка может требоваться "как есть".** Для
   `select x, x*2 as y from t` обе projection-колонки (`x` и `y`) читают одну
   и ту же входную колонку `x`. Если "вычислить `y` in-place поверх `x`",
   значение исходного `x` будет потеряно для соседней projection. Т.е. решение
   "in-place или нет" нельзя принимать только по одной projection — нужно
   смотреть на весь список.
3. **Чужой буфер — чужое владение.** Даже если входная колонка — это выход
   ПРЕДЫДУЩЕГО `Project`-узла (уже не Arrow, а собственный malloc'нутый
   буфер), этот буфер принадлежит rowset'у предыдущего узла
   (`Retain`/`Release`, `TRowSet::Destroy`). Текущий узел держит на него только
   ссылку через `Retain`. Перезаписать его — значит молча сломать инвариант
   владения соседнего узла (а в будущем, после Stage 3 этого плана и/или
   `PLAN_JOIN.md`, тот буфер может быть переиспользуемым между батчами или
   удерживаемым дольше одного `Next()`).

### Что тогда означает "in-place" — per-column alias

Для **identity**-projection (`select x` / `select x as y`, `Expression` —
голый `TIdentExpr`, имя есть во входной схеме) "in-place" уже реализовано и
работает: `TRuntimeProject::Next` копирует **структуру** `TColumn` (указатели
`Data`/`Mask`/`Offsets` как есть) в новый `std::vector<TColumn>` и держит вход
живым через `Retain`/`Release` (`TProjectedRowSetData`, `project_exec.cpp`).
Ни один байт данных не копируется и не трогается — это и есть "in-place" в
правильном смысле: **alias**, а не "переписать значения".

Новый узел этого плана (Stage 1) — **гибрид**: для каждой выходной колонки
независимо —

- `Expression` — identity (голый `TIdentExpr`, имя есть во входной схеме) →
  **alias**: копия `TColumn` как сегодня, без выделения памяти;
- иначе → **materialize**: свежий `Data`(`/Mask`/`Offsets`) буфер размера
  батча, заполняется сгенерированным построчным ядром.

Список из примера `select x, x*2 as y from t` даёт ОДНУ materialized колонку
(`y`) и ОДНУ aliased (`x`); `y`'s буфер новый, `x` остаётся ссылкой на
исходные данные.

### Что тогда означает "in-place" — переиспользование буфера между батчами

Настоящая перезапись "in place" (без новой аллокации malloc на каждый
`Next()`) возможна только как **внутренняя оптимизация одного узла**:
materialize-колонка узла может держать СВОЙ буфер от предыдущего батча и
писать в него поверх старых значений на следующем `Next()`. Это безопасно
ТОЛЬКО если предыдущий выданный `TRowSet` уже полностью прочитан и
`Release`'нут (refcount вернулся к нулю) до следующего `Next()` — то есть при
инварианте "не более одного батча этого узла в полёте", который сегодня верен
для всех потоковых 1:1 узлов (`Filter`, текущий `Project`), но НЕ
гарантирован архитектурно и будет нарушен любым будущим pipeline-breaker'ом,
который буферизует несколько батчей (`PLAN_JOIN.md`'s `TRowStore`,
будущие window-функции). Это Stage 3 — оптимизация, явный opt-in, документируется
отдельно.

**Итог:** Stage 1-2 всегда аллоцируют новый буфер на каждый `Next()` для
materialize-колонок (как `SelectionBuf_` в `TRuntimeFilter`, но без
переиспользования) — корректно и просто. Stage 3 добавляет переиспользование
буфера для узлов, про которые известно, что предыдущий выход уже `Release`'д.

---

## Правила изменений `externals/qumir`

Тот же набор правил, что в `PLAN_AGGREGATION.md`/`PLAN_JOIN.md` — изменения
сабмодуля допустимы только additive/минимальные и по отдельному согласованию.
**Ожидание:** этот план не требует изменений `externals/qumir` — переиспользует
`AllowOverloads`, `cast`, битовые операции (`unsigned_bit_ops.oz`), которые уже
включены и работают для filter/aggregation.

---

## Архитектурные опорные точки (из исследования)

- **`TProjectOperator`/`MakeProject`** (`qdb/ops/project.{h,cpp}`) — уже общий:
  `Projections(): vector<TProjectionSpec{Name, TExprPtr}>`,
  `ComputeReferencedColumns()` = `⋃ FindUnboundVars(spec.Expression)`. Менять
  не нужно.
- **Sexp** (`qdb/sexp/{printer,parser}.cpp`) — `(rel project <input> (name
  <expr>)...)` уже печатает/парсит произвольный `<expr>` через
  `PrintAst`/`h.Expr()`. Менять не нужно.
- **`pipeline/typing.cpp`** — для `TProjectOperator` нужно посчитать тип
  каждого `spec.Expression`, не только identity (см. Stage 1 §1).
- **`pipeline/column_pruning.cpp`** — уже использует `ComputeReferencedColumns`
  как "required" для `Project`/`Aggregate` — для materialize-колонок это и
  так все входные идентификаторы из выражения, ничего менять не нужно.
- **`GenFilterKernelAst`** (`qdb/kernel/gen.cpp:656`) — образец построчного
  векторизованного ядра над `(ref TRowSet)`: типизированные указатели для
  fixed-width колонок (`cast(cast(Data,i64),<ptr T>)`), `StringView` для
  строковых через `BuildColumnValueAst`, цикл `i in [0,n)`, запись результата
  через `TArrayAssignExpr`. Ядро проекции — структурно тот же цикл, но:
  - читает входные колонки (как filter — `BuildColumnValueAst`),
  - пишет M выходных колонок (новый "write"-аналог, см. Stage 1 §3).
- **`SubstFieldsInPlace`** (`qdb/kernel/gen.cpp:633`) — рекурсивно заменяет
  `TIdentExpr("x")` → `(index x i)` по всему дереву выражения. Переиспользуется
  как есть для каждого materialize-выражения.
- **`BuildColumnValueAst`** (`qdb/kernel/column_value.{h,cpp}`) — построчное
  ЧТЕНИЕ значения колонки (`Setup`, `Value`, `IsValid`, `ValueType`) для
  int/float/bool/string с проверкой null-маски через `bitoff`. Это "read"-половина
  того, что нужно ядру проекции; "write"-половина — новая (Stage 1 §3).
- **`TAggregateMeasure`/`TAggregateFinalize`** (`qdb/kernel/compiler.h`,
  `GenGenericAggregateMeasureAst`/`GenGenericAggregateFinalizeAst`) — образец
  two-pass "посчитать нужный размер → аллоцировать → заполнить" для
  переменной длины. Нужен в Stage 2 для строковых вычисляемых колонок.
- **`qdb_alloc`/`qdb_realloc`/`qdb_free`** (`qdb/modules/qumirdb_runtime.cpp` +
  `qumirdb.cpp::ExternalFunctions_`) — уже зарегистрированные внешние
  примитивы, доступны из generated-кода. Для Stage 1 не нужны — буферы
  аллоцирует C++ (`exec`), как `SelectionBuf_` в `TRuntimeFilter`.
- **`bitoff`** (`qumirdb.cpp:172`, внешняя функция) — читает бит из bitmap.
  Только чтение. Для записи bool-значений и null-масок (Stage 1 §3) нужен
  встречный helper — но как **сгенерированная per-query oz-функция**
  (аналог `GenReducerFunDecls`), не как изменение `externals/qumir`.
- **Nullability — только runtime-факт**, не часть `TTypePtr`
  (`io/schema.h::TColumnSchema{Name, Type}` — типа `bool`, без флага
  nullable). Значит generated-ядро ВСЕГДА должно уметь читать `IsValid` для
  входных колонок (что `BuildColumnValueAst` и делает) и писать output-маску —
  без compile-time различения "эта колонка точно NOT NULL".

---

## Stage 1 — числовые/bool вычисляемые колонки (с nullability)

Цель: `select x*2 as y, x as z from t` — `y` materialized (i64, маска = маска
`x`), `z` aliased.

### 1. `pipeline/typing.cpp` — типы вычисляемых полей

Сейчас для не-identity projection `fieldType = nullptr`. Нужна функция

```cpp
// qdb/pipeline/expr_typing.{h,cpp} (новый файл)
NQumir::NAst::TTypePtr InferProjectionType(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType);
```

Подход — мини-инференс по поддерживаемому подмножеству, **повторяющий правила
самого Qumir** (а не придумывающий новые):

- `TIdentExpr("x")` → тип поля `x` из `inputType` (как сегодня для identity).
- `TNumberExpr`/`TBoolExpr`/`TStringExpr` → тип литерала как в Qumir-парсере.
- `TCastExpr(_, T)` → `T`.
- `TUnaryExpr("-"/"!" , e)` → тип `e` (для `!` — `bool`).
- `TBinaryExpr(op, l, r)`:
  - арифметика (`+ - * / %`) над int/int или float/float → правило
    промоушна ширины/знаковости, которое уже есть в Qumir и было исправлено
    для bitwise (`PLAN_AGGREGATION.md`, "unsigned_bit_ops.oz") — **взять то же
    правило** (см. `externals/qumir/qumir/semantics/type_annotation/`, искать,
    нет ли там экспортируемого helper'а для "результат бинарной арифметики
    over (T1,T2)"; если нет — узко повторить для `{i8..i64,u8..u64,f64}`);
  - сравнения (`== != < <= > >=`) → `bool`;
  - логика (`&& ||`) → `bool`.
- Иначе (вызовы функций, строковые операции, generic) — НЕ Stage 1; до
  Stage 2 такие projection-выражения остаются "not implemented yet"
  (как сегодня), но теперь точечно — только для конкретного выражения, а не
  для всего `TProjectOperator`.

Если когда-нибудь подмножество станет слишком большим/рассинхронизируется с
реальным Qumir — запасной путь: собрать крошечную "typing program"
(`(fun probe (<inputStruct>) -> <anon> (block (return (struct ... projN))))`),
прогнать через `NQumir::NTypeAnnotation::TTypeAnnotator` с тем же
`TNameResolver`/`AllowOverloads`, что и `CompileKernelAst`, и забрать
`->Type` у каждого `projN`. Не делать это по умолчанию — лишняя
resolver/JIT-инфраструктура на каждый `AnnotateTypes`.

`AnnotateTypes` для `TProjectOperator`: для identity — как сегодня; для
прочих — `InferProjectionType(...)`; если `nullptr` — оставить как сегодня
(ошибка на стадии `planner::Build`, текст уточнить: "unsupported projection
expression: <name>").

### 2. `qdb/kernel/gen.cpp` — `GenProjectKernelAst`

```cpp
// Builds a vectorized projection kernel: (fun <kernel>
//   ((var in <ref TRowSet>) (var out <ref TRowSet>)) -> void)
// `materialized` lists only the NON-alias output columns: for each, the
// 0-based index into out.Columns[], its substituted expression, and its
// output TTypePtr (from typing). `fieldIndices`/`inputType` describe `in`,
// exactly as for GenFilterKernelAst.
NQumir::NAst::TExprPtr GenProjectKernelAst(
    std::vector<TMaterializedProjection> materialized,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType);
```

Тело — тот же скелет, что `GenFilterKernelAst`:

- `n = in.RowCount` (== `out.RowCount`, гарантируется вызывающим C++);
- для каждого поля `inputType`, на которое ссылается ХОТЯ БЫ ОДНО
  materialize-выражение (через `FindUnboundVars`): bind как у filter —
  типизированный `<ptr T>` для fixed-width, `StringView`+`Setup` через
  `BuildColumnValueAst` для строк;
- для каждой materialize-колонки `j`: bind типизированный `<ptr Tj>` из
  `out.Columns[j].Data` (`cast(cast(Data,i64),<ptr Tj>)`), и — если `Tj` это
  `bool` — отдельно `<ptr u8>` для bitmap-записи значения;
- если у входного поля, использованного в выражении `j`, бывает null —
  bind `<ptr u8>` из `out.Columns[j].Mask` (выделяется C++ всегда для
  materialize-колонок в Stage 1 — см. §5; "всегда выделять" проще, чем
  доказывать на этапе typing, что null невозможен);
- цикл `i in [0,n)`:
  - для каждого `j`: `value_j = <substituted expr_j>` (через
    `SubstFieldsInPlace(expr_j, fixedFields, identI)` + строковые идентификаторы
    как у filter);
  - `valid_j = && по IsValid всех идентификаторов, упомянутых в expr_j`
    (из `BuildColumnValueAst.IsValid`; если у поля `Mask == nullptr`,
    `IsValid` тривиально true — не требует доп. кода, т.к.
    `BuildColumnValueAst` уже сводит это в одно выражение);
  - запись значения: fixed-width не-bool — `out_j[i] = cast(value_j, Tj)`
    (`TArrayAssignExpr`, как `selection[i]=...` у filter); bool —
    `qdb_set_bit(out_data_bitmap_j, i, value_j)`;
  - запись маски: `if valid_j: qdb_set_bit(out_mask_j, i, #t)` (буфер маски
    выделяется обнулённым — см. §5 — поэтому "невалидно" — это просто "бит не
    установлен", clear-ветка не нужна, не требуется bitwise NOT).

`qdb_set_bit` — сгенерированная per-query oz-функция (одна штука на ядро,
как `GenApplyReducersFunDecl`):

```lisp
(fun qdb_set_bit ((var bitmap <ptr u8>) (var index i64) (var value bool)) -> void
  (block
    (if value
      (block
        (var byte_index = (>> index (: 3 i64)))
        (var bit_pos = (& index (: 7 i64)))
        (var old = (cast (index bitmap byte_index) i64))
        (= bitmap [byte_index] (cast (| old (<< (: 1 i64) bit_pos)) u8)))
      (block))))
```

### 3. `qdb/kernel/column_value.{h,cpp}` — без изменений

`BuildColumnValueAst` уже даёт всё нужное для ЧТЕНИЯ (`Value`, `IsValid`).
"Write"-сторона — не отдельный generic-helper, а прямые
`TArrayAssignExpr`/`qdb_set_bit` в `GenProjectKernelAst` (см. §2) — она проще
read-стороны (нет работы с offsets/переменной длиной в Stage 1) и не требует
отдельной абстракции.

### 4. `qdb/kernel/compiler.{h,cpp}` — `CompileProject`

```cpp
struct TMaterializedProjection {
    int32_t OutputIndex;             // index into output TRowSet.Columns
    NQumir::NAst::TExprPtr Expression; // unbound, references input field names
    NQumir::NAst::TTypePtr Type;       // from InferProjectionType
};

// project_dispatch(ref TRowSet in, ref TRowSet out) -> void
using TProjectDispatch = std::function<void(TRowSet& in, TRowSet& out)>;

TProjectDispatch CompileProject(
    const NQumir::NAst::TStructType& inputType,
    std::vector<TMaterializedProjection> materialized);
```

Реализация — калька `CompileFilter`: `GenProjectKernelAst` → опционально
строковая библиотека (`filter_string_ops.oz`, если среди входных полей есть
строки) → `CompileKernelAst("<kernel>")` → `TProjectDispatch` лямбда,
вызывающая `void(*)(void*,void*)` с `&in, &out`.

### 5. `qdb/exec/project_exec.{h,cpp}` — `TRuntimeComputedProject`

Новый класс (старый `TRuntimeProject` остаётся для чисто-identity случая —
см. §6):

```cpp
struct TProjectionPlanItem {
    enum class EKind { Alias, Materialize };
    EKind Kind;
    int32_t SourceIndex = -1; // Alias: index into input.Columns
};

class TRuntimeComputedProject : public IRuntimeNode {
public:
    TRuntimeComputedProject(
        std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        std::vector<TProjectionPlanItem> plan,
        std::vector<TMaterializedProjection> materialized, // for sizing
        TKernelCompiler::TProjectDispatch dispatch);

    bool Next(TRowSet& rowSet) override;
private: ...
};
```

`Next()`:

1. `Input_->Next(input)`; `n = input.RowCount`.
2. Для каждого materialize-`j` в `plan`: посчитать размер элемента из его
   `TTypePtr` (`i8..i64,u8..u64` → 1/2/4/8 байт; `f64` → 8; `bool` → bit-packed,
   `ceil(n/8)` байт); аллоцировать `Data` буфер `n * elemSize` (или
   `ceil(n/8)` для bool) И `Mask` буфер `ceil(n/8)` байт, **обнулённый**
   (`std::vector<uint8_t>` с `resize` — обнуляет по умолчанию). `Offsets =
   nullptr`, `OffsetWidth = 0`, `DataBitOffset = 0` (bool-Data — отдельный
   bit-packed буфер с `DataBitOffset=0`, как Arrow boolean array).
3. Для каждой `Alias`-`j` в `plan`: `out.Columns[j] = input.Columns[SourceIndex]`
   (копия структуры, как сегодня в `TRuntimeProject`).
4. Собрать `out` (`RowCount = n`, `Selection = input.Selection` — проекция не
   меняет состав строк, маска выбора того же размера и смысла).
5. `Dispatch_(input, out)` — заполняет materialize-колонки.
6. `Destroy`: `Release(&input)` + освободить собственные буферы (хранятся в
   приватной структуре, аналог `TProjectedRowSetData`, но с
   `vector<vector<uint8_t>> OwnedData, OwnedMask`).

Буферы **не переиспользуются** между вызовами `Next()` в Stage 1-2 (см.
"Главный вопрос" — Stage 3 это меняет).

### 6. `qdb/exec/planner.cpp` — wiring

```cpp
if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
    auto project = maybe.Cast();
    auto input = Build(project->Input());
    auto* inputType = static_cast<TStructType*>(input->OutputType().get());

    std::vector<TProjectionPlanItem> plan;
    std::vector<TMaterializedProjection> materialized;
    bool allAlias = true;
    for (size_t j = 0; j < project->Projections().size(); ++j) {
        const auto& p = project->Projections()[j];
        if (auto ident = TMaybeNode<TIdentExpr>(p.Expression)) {
            auto it = find_field(inputType, ident.Cast()->Name);
            if (it != end) {
                plan.push_back({Alias, index_of(it)});
                continue;
            }
        }
        allAlias = false;
        auto* outStruct = static_cast<TStructType*>(project->OutputColumns().get());
        plan.push_back({Materialize, -1});
        materialized.push_back({(int32_t)j, ClonePredicate(p.Expression), outStruct->Fields[j].second});
    }

    if (allAlias) {
        // existing zero-copy path, unchanged
        return std::make_unique<TRuntimeProject>(std::move(input), project->OutputColumns(), ...);
    }

    TKernelCompiler compiler(Diagnostics_);
    auto dispatch = compiler.CompileProject(*inputType, materialized);
    return std::make_unique<TRuntimeComputedProject>(
        std::move(input), project->OutputColumns(), std::move(plan),
        std::move(materialized), std::move(dispatch));
}
```

Существующий identity-only путь (`allAlias == true`) остаётся прежним —
нулевая регрессия для `select a, b, c`.

### 7. CMake / тесты

- `test/test_project_kernel.cpp` (новый, аналог
  `test/test_filter_kernel.cpp`, если такой есть — иначе уровень
  `CompileProject` напрямую): `i64`/`f64`/`bool` арифметика, с `Mask` и без,
  один и несколько выходов, смесь alias+materialize.
- `test/test_pipeline.cpp` или аналог: `AnnotateTypes` для не-identity
  projection даёт правильный `TTypePtr`.
- e2e: sexp `(rel project (rel source ...) (y (* x (: 2 i64))) (x x))` →
  planner → `Next()` → проверить значения и маску `y`.

---

## Stage 2 — строковые / переменной длины вычисляемые колонки

Намётка (не блокирует Stage 1):

- Появляются projection-выражения вида `upper(name)`, `concat(a,b)`,
  `substr(s, from, len)` — Qumir/qdb пока не имеет таких операций
  (`qdb/kernel/aggregation/string_ops.oz` содержит только hash/equal/compare
  для ключей агрегации, не текстовые трансформации) — сначала нужно решить,
  где живут сами текстовые функции (отдельная `.oz`-библиотека по аналогии с
  `string_ops.oz`, плюс `qdb_alloc`-based буфер для результата).
- Размер результата заранее неизвестен (например, `concat(a,b)` →
  `len(a)+len(b)`, но `upper(a)` → `len(a)`, а гипотетический `repeat(a,n)` —
  произведение). Нужен **two-pass**, по образцу
  `TAggregateMeasure`/`TAggregateFinalize`:
  - pass 1 (`measure`): по каждой строке посчитать длину результата, накопить
    сумму → C++ аллоцирует `Data` (сумма) + `Offsets` (`n+1` элементов,
    `OffsetWidth=4` или `8`);
  - pass 2 (`finalize`): повторно вычислить выражение для каждой строки и
    записать байты в `Data[Offsets[i]..Offsets[i+1])`, заполнить `Offsets`.
- `GenProjectKernelAst` для строковых materialize-колонок генерирует ДВА тела
  (`measure`/`write`) вместо одного цикла — расширение `CompileProject` до
  `TProjectKernels{Measure, Write}`, аналогично `TAggregateKernels`.
- `TRuntimeComputedProject::Next()` для строковых колонок: вызвать `Measure`,
  аллоцировать `Data`/`Offsets` по результату, вызвать `Write`.

---

## Stage 3 — переиспользование буферов между батчами (настоящий "in-place")

Опциональная оптимизация после Stage 1-2 корректности. Условия безопасности
(см. "Главный вопрос"):

- инвариант "не более одного выданного батча этого узла в полёте" — верен для
  текущих 1:1 потоковых узлов; должен быть явно зафиксирован как контракт
  `IRuntimeNode` (комментарий в `executor.h`) и проверяться через
  `RefCount`/assert в debug-сборке (`assert(prevOut.RefCount == 0)` перед
  переписью буфера на новом `Next()`);
- буфер переиспользуется только если НОВЫЙ размер (`n * elemSize`,
  `ceil(n/8)`) `<=` размеру предыдущей аллокации — иначе `qdb_realloc`
  (уже зарегистрирован) или новая аллокация;
- реализация: `TRuntimeComputedProject` хранит `OwnedData`/`OwnedMask` как
  поля экземпляра (не в `Private` каждого rowset'а), `resize()` вместо
  пересоздания; `Destroy` для предыдущего rowset'а становится no-op для этих
  буферов (они принадлежат узлу, не rowset'у) — НО тогда `Destroy` обязан
  по-прежнему `Release(&input)`.
- если/когда появится pipeline-breaker, удерживающий несколько батчей
  (`PLAN_JOIN.md` `TRowStore`), для узлов ПЕРЕД ним эта оптимизация должна
  быть выключена (или буферы должны клонироваться при retain) — зафиксировать
  как явную заметку в `executor.h` на момент реализации Join.

---

## Детальный порядок реализации

### A. Зафиксировать ограничения Stage 1
- [ ] Поддерживаемые выражения: `+ - * / % == != < <= > >= && || !` унарный
  минус, `cast`, литералы, идентификаторы — над `i8..i64,u8..u64,f64,bool`.
  Явная ошибка ("unsupported projection expression") для всего остального
  (строки, вызовы функций) — задел для Stage 2.

### B. `InferProjectionType` + `pipeline/typing.cpp` (Stage 1 §1)
- [ ] Новый файл `qdb/pipeline/expr_typing.{h,cpp}`.
- [ ] `AnnotateTypes` для `TProjectOperator` использует его для не-identity
  полей.
- [ ] Unit-тест: типы для `+ - * / cast == < ...` над разными int/float
  комбинациями совпадают с тем, что выдаёт `CompileKernelAst`'s annotator на
  эквивалентном вручную написанном `.oz` (кросс-проверка).

### C. `GenProjectKernelAst` + `qdb_set_bit` (Stage 1 §2)
- [ ] `(ref TRowSet in, ref TRowSet out) -> void`, читает через
  `BuildColumnValueAst`/`SubstFieldsInPlace`, пишет через
  `TArrayAssignExpr`/`qdb_set_bit`.
- [ ] Standalone тест: AST строится и компилируется через
  `TLLVMRunner::CompileKernelAst` на руками собранных `TRowSet` буферах
  (как существующие kernel-level тесты для filter/aggregation).

### D. `CompileProject` (Stage 1 §4)
- [ ] `qdb/kernel/compiler.{h,cpp}`: `TMaterializedProjection`,
  `TProjectDispatch`, `CompileProject`.

### E. `TRuntimeComputedProject` (Stage 1 §5)
- [ ] `qdb/exec/project_exec.{h,cpp}`: план alias/materialize, аллокация
  `Data`/`Mask` буферов, `Destroy`.

### F. Planner wiring (Stage 1 §6)
- [ ] `exec/planner.cpp`: `allAlias` fast-path сохранён; иначе
  `CompileProject` + `TRuntimeComputedProject`.

### G. Тесты / CMake (Stage 1 §7)
- [ ] `test/test_project_kernel.cpp` + e2e sexp/CLI на parquet с nullable
  колонкой: `select price, price * (: 2 f64) as doubled from t where qty < 100`
  — сверка с DuckDB.

### H. Stage 2 (строки) — намётка, без чеклиста до решения по текстовым функциям.

### I. Stage 3 (переиспользование буферов) — намётка, после A-G и явного
  контракта single-in-flight в `executor.h`.

---

## Проверка

- `cmake --build build -j` чистый; `ctest --output-on-failure` зелёный.
- Stage 1: `select <arith/cmp expr> as y, <ident> as z from t [where ...]` —
  значения и null-маска `y` совпадают с эталоном (ручной C++ расчёт по тем же
  входным буферам), `z` — байт-в-байт alias входной колонки (тот же указатель
  `Data`).
- Регрессия: существующие чисто-identity `select a, b, c` продолжают идти по
  старому `TRuntimeProject` (без изменений в сгенерированном плане/диагностике
  `PrintRuntimePlan`).
- E2E (после Stage 1): CLI на `orders.parquet`, `select qty, qty * 2 as
  qty2 from orders where qty < 100`, diff с DuckDB по тому же запросу — пустой.
