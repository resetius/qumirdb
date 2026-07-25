# План: оператор Aggregation (Robin Hood hash + generic-ядро агрегации)

## Context

В qumir теперь есть **перегрузки** (`pragma language overloads`, `OverloadSets`,
`LookupOverloads`) и **шаблоны** (`<named T (template readable mutable)>`). Это
позволяет реализовать в qdb новый реляционный оператор `Aggregation`
(SQL GROUP BY + агрегаты count/sum/min/max) поверх **Robin Hood хэш-таблицы**,
написанной как переиспользуемые generic-функции на oz-lang.

Принципиальное решение: функции Робин-Гуда и ядро агрегации **встраиваются как
исходный текст Qumir через `R"__(...)__"`**, парсятся штатным core-парсером и
вставляются AST-узлами в верхний блок ядра (инъекция в ядро, в обход регистрации
каждой функции в модуле). Память (рост хэш-таблицы, буферы результата) выделяется
через **внешние alloc-функции, добавляемые в модуль QumirDb** и резолвящиеся JIT'ом
по символам процесса.

Существующий приём в `qdb/ops/parse_kernel.cpp::WrapInFunction` — ровно этот
паттерн (собрать исходник строкой → распарсить → взять AST). На него опираемся.

**Этапность:**
- **Stage 1 — только целочисленные ключи** (fixed-width). Цель этого плана.
- **Stage 2 — строки и сложные данные** (str_view/str_hash из `PLAN_OVERLOADS.md`)
  — отдельный этап, здесь только намечается.

**Текущий статус:** Stage 1 доведён до `TRuntimeAggregate`, planner, sexp E2E и
CLI/parquet, но production kernel всё ещё concrete для `Key=i64`. Следующая цель
описана в разделе **M**: собирать один typed AST из generic `.oz` библиотек,
внедрённых key operations/reducers и generated wrapper, после чего прогонять
весь блок через обычный Qumir resolver/type annotation/lowering pipeline.

## Правила изменений `externals/qumir`

- [x] По умолчанию qdb адаптируется к существующим parser/runner/lowering API и
  их текущему поведению.
- [x] В `externals/qumir` допустимы только минимальные локальные изменения,
  которые не меняют поведение существующих программ и тестов: additive option,
  узкий bugfix или недостающий API с сохранением старого контракта.
- [x] Нельзя без отдельного согласования менять выбор entry point, порядок
  функций, ABI JIT-вызова, правила parsing/type annotation/lowering или другие
  существующие семантические контракты.
- [x] Если для aggregation требуется изменение с заметным blast radius, работа
  останавливается на отдельном воспроизводимом тесте; предлагаемая правка и её
  последствия сначала согласовываются отдельно.
- [x] Любая согласованная правка сабмодуля оформляется отдельным коммитом в
  `externals/qumir`, после чего отдельно обновляется указатель сабмодуля в qdb.
- [x] Исправлена потеря width/signedness результата bitwise operators в Qumir;
  добавлен `corelang/unsigned_bit_ops.oz`, полный regtest проходит.

---

## Архитектурные опорные точки (из исследования)

- **Оператор** = `IOperator : TExpr` (`qdb/ops/operator.h`). Несёт `Type =
  TFunctionType([requiredInput], output)`; `OutputColumns()` / `RequiredColumns()`
  читают `ReturnType` / `ParamTypes[0]`. Образцы: `TFilterOperator`,
  `TProjectOperator`.
- **Sexp** печать/разбор: `qdb/sexp/printer.cpp` (`PrintRel`), `qdb/sexp/parser.cpp`
  (`MakeRelParsers`) — диспетчеризация по `RelName()`.
- **Pipeline**: `pipeline/typing.cpp::AnnotateTypes` (bottom-up, ставит
  `TFunctionType`), `pipeline/column_pruning.cpp`, `pipeline/unbound_vars.cpp`.
- **Kernel**: `kernel/gen.cpp` строит AST ядра (скелет зависит от рантайм-схемы:
  индексы колонок, типы), `kernel/compiler.cpp::CompileFilter` компилит через
  `TLLVMRunner::CompileKernelAst`, регистрируя `QumirDbModule`.
- **Exec**: `IRuntimeNode { OutputType(); Next(TRowSet&) }` (`exec/executor.h`).
  Filter/Project — стриминговые 1:1. `project_exec.cpp` показывает паттерн
  **аллокации собственного выходного rowset** с кастомным `Destroy`/`Private`
  (`TProjectedRowSetData`). Планировщик: `exec/planner.cpp::Build`.
- **Данные**: `io/io.h` — `TColumn{Data,Mask,Offsets,...}`, `TRowSet{Columns,
  ColumnCount,RowCount,Selection,Destroy,Private,RefCount}` + `Retain/Release`.
- **Модуль**: `qdb/modules/qumirdb.cpp` — `ExternalTypes_` (TColumn/TRowSet с
  байтовыми layout'ами), `ExternalFunctions_` (пример inline-фабрики `bitoff`).
  Внешние функции с реальным C++-указателем (`system.cpp::sqrt`): `.Ptr` +
  `.Packed` + `.MangledName`; JIT резолвит по символу процесса
  (`llvm_runner.cpp:145 LoadLibraryPermanently(nullptr)`).
- **Перегрузки/шаблоны**: включаются `AllowOverloads` на `TNameResolver`
  (`name_resolver.cpp::ApplyPragmas`). Но `CompileKernelAst`/`CompileKernel`
  **не** вызывают `ApplyPragmas`, а `TLLVMRunner::Resolver` создаётся с дефолтными
  опциями → перегрузки/generics в ядрах сейчас недоступны без правки.

**Aggregation — pipeline breaker.** В отличие от filter/project, рантайм-узел
должен втянуть ВЕСЬ вход (`while(Input_->Next(batch))`), на каждом батче гнать
update-ядро (build/probe хэш-таблицы), и лишь по исчерпании входа отдать результат
из таблицы (один или несколько выходных rowset'ов на `Next()`).

---

## Изменения в сабмодуле qumir (минимальные)

`externals/qumir/qumir/runner/runner_llvm.{h,cpp}`:
- Добавить в `TLLVMRunnerOptions` поле `bool AllowOverloads = false`.
- В конструкторе `TLLVMRunner` при `Options.AllowOverloads` вызвать
  `Resolver.ApplyPragmas({ NAst::TPragma{"language", {"overloads"}, {}} })`
  до регистрации модулей (или сразу после конструирования `Resolver`).
- Этого достаточно: `Declare`/`LookupOverloads`/инстанцирование шаблонов в
  `type_annotation.cpp` уже работают, как показывают regtest'ы
  (`test/regtest/cases/corelang/generic_*.oz`).

Коммитить как отдельный коммит в сабмодуле, затем сдвинуть указатель в родителе.

---

## Stage 1 — реализация (integer keys)

### 1. Логический оператор — `qdb/ops/aggregate.{h,cpp}`

```cpp
struct TAggregateSpec {
    std::string Name;            // имя выходной колонки
    std::string Func;            // "count" | "sum" | "min" | "max"
    NQumir::NAst::TExprPtr Arg;  // выражение-аргумент (ident колонки), nullptr для count(*)
    // На этапе kernel generation builtin Func разворачивается в Oz reducer AST.
    // Позже здесь может храниться непосредственно custom reducer function AST.
};

class TAggregateOperator : public IOperator {
    static constexpr const char* OpId = "aggregate";
    // ctor(input, groupKeys: vector<string>, aggs: vector<TAggregateSpec>)
    // RelName(), Children()={Input_}, ToString()
    // ComputeReferencedColumns() = groupKeys ∪ ⋃ FindUnboundVars(agg.Arg)
    // Type: ParamTypes[0]=input.OutputColumns() (полная схема, сужается pruning'ом),
    //       ReturnType = struct{ groupKey-колонки + по одной на каждый агрегат }
private:
    TOperatorPtr Input_;
    std::vector<std::string> GroupKeys_;
    std::vector<TAggregateSpec> Aggs_;
};
// MakeAggregate(input, keys, aggs-as-strings) — парсит arg-выражения как в MakeProject.
```

Тип выхода: для groupKey-колонки берём тип из входной struct по имени; для агрегата
выводим тип по `Func` (`count`→i64; `sum/min/max`→тип аргумента; `sum` целых→i64).

### 2. Sexp — `qdb/sexp/{printer,parser}.cpp`

Синтаксис:
```
(rel aggregate <input>
  (keys k1 k2 ...)
  (agg out_name func arg_expr) ...)
```
- В `MakeRelParsers`: добавить ветку `nameTok.Name == TAggregateOperator::OpId`
  по образцу project — разобрать `(keys ...)`, затем список `(agg name func expr)`.
- В `PrintRel`: симметричная печать.

### 3. Pipeline — typing / pruning

- `pipeline/typing.cpp::AnnotateTypes`: ветка `TMaybeOp<TAggregateOperator>` —
  поставить `TFunctionType([inputOutput], aggOutputStruct)` (логику типов из п.1
  вынести в оператор/общий хелпер, как у project).
- `pipeline/column_pruning.cpp`: aggregate — pruning-барьер; нужные входу колонки =
  `ComputeReferencedColumns()` (ключи + аргументы агрегатов), не зависит от
  родителя. Аналогично project в существующем проходе.
- `unbound_vars.cpp` уже готов (используется через `ComputeReferencedColumns`).

### 4. Робин-Гуд и агрегация как отдельно тестируемый oz-lang код

До интеграции с pipeline исходники разрабатываются в
`qdb/kernel/aggregation/*.oz` и запускаются отдельным
`test/test_aggregation.cpp`. C++ в этом контуре только:

- читает `.oz` файлы и передаёт их в `TLLVMRunner`;
- регистрирует `QumirDbModule`, когда тесту нужны внешние типы/alloc-функции;
- подготавливает входные буферы, вызывает JIT entry point и проверяет результат.

Hash, probing, lookup, insertion, displacement, rehash и обновление агрегатов
реализуются только на oz-lang. Reference-реализации этих алгоритмов на C++ не
делаем.

Hash table orchestration не знает семантику `count/sum/min/max`: она возвращает
stable `SlotId` и признак новой группы, читает предыдущее состояние из aggregate
buffer и вызывает reducer-функцию, переданную/generated в общем AST:

```text
reduce(State prev, Value value, bool is_new) -> State
```

`Value` — типизированный результат `TAggregateSpec::Arg` для текущей строки, а не
жёстко зафиксированный runtime row ABI. Builtin `count/sum/min/max` являются
синтаксическим сахаром, который генерирует обычные Oz reducer functions. Custom
aggregation позже сможет передать собственный reducer AST с тем же контрактом.
`is_new` обязателен для корректной first-value initialization без sentinel.

Generic-граница задаётся сразу:

```text
<named Key (template readable mutable)>
```

Алгоритм таблицы работает с `Key`, а представление ключа скрыто за overload-
контрактом:

```text
rh_hash(Key) -> i64
rh_key_equal(Key, Key) -> bool
rh_key_copy(Key) -> Key        // только если копирование нельзя выразить присваиванием
```

Stage 1 предоставляет только overload для `Key=i64`. Следующий тип ключа
добавляется новыми overload-функциями, без изменения probing/insertion кода.
Первая рабочая таблица поддерживает один scalar `i64` key; составной ключ из двух
`i64` добавляется отдельным шагом после стабилизации scalar-варианта.

**4b. Внешние alloc-функции в QumirDb** — `qdb/modules/qumirdb.cpp`
(`ExternalFunctions_`), бэкенд — `extern "C"` символы qdb (новый
`qdb/modules/qumirdb_runtime.cpp`):
```
qdb_alloc(i64 nbytes) -> <ptr i8>      // backed by std::malloc/арена
qdb_realloc(<ptr i8>, i64) -> <ptr i8>
qdb_free(<ptr i8>)
```
Регистрация по образцу `system.cpp::sqrt`: `.MangledName`, `.Ptr`, `.Packed`,
`.ArgTypes`, `.ReturnType`. Символы должны экспортироваться (на Linux —
проверить/добавить `-rdynamic` для bin/qdb и тестов; на macOS работает).
Добавить файл в `qdb/CMakeLists.txt`.

**4c. Генератор ядра агрегации** — `kernel/gen.{h,cpp}` (только после завершения
standalone-тестов):
`GenAggregateKernelAst(inputType, fieldIndices, groupKeyFields, aggSpecs, htType,
columnType, rowSetType)`:
- скелет per-query как AST (зависит от рантайм-схемы): извлечение `n`, `cols`,
  типизированных указателей колонок (повторно использовать `SubstFieldsInPlace`/
  логику `GenFilterKernelAst`);
- цикл по строкам: вычислить ключ, `rh_get_or_init` возвращает `(SlotId, isNew)`,
  затем для каждого агрегата wrapper вычисляет `Arg`, читает `prev` и вызывает
  соответствующую Oz reducer function; hash table код не ветвится по имени
  `count/sum/min/max`;
- встроить уже протестированные `.oz`-блоки: распарсить их core-парсером,
  сконкатенировать `Stmts` с FunDecl'ами generated wrapper в один верхний блок;
  entry point (`agg_update`/`agg_finalize`) должен быть последним.

**4d. Компилятор** — `kernel/compiler.{h,cpp}`:
`CompileAggregate(...)` по образцу `CompileFilter`, но:
- `opts.AllowOverloads = true` (новая опция qumir);
- вернуть update-ядро `void agg_update(TRowSet* batch, HashTable* ht)`;
  update и финализация таблицы остаются oz-lang ядрами; C++ runtime-node только
  управляет их вызовом и lifetime rowset.

> Примечание: HashTable как структура — зарегистрировать `TExternalType` в QumirDb
> (layout: указатели на буферы, capacity, size, load factor), чтобы и Qumir-ядро,
> и C++-рантайм видели одинаковый layout (как TColumn/TRowSet).

### 5. Физический узел — `qdb/exec/aggregate_exec.{h,cpp}`

```cpp
class TRuntimeAggregate : public IRuntimeNode {
    // ctor(input, outputType, updateDispatch, htConfig)
    bool Next(TRowSet& out) override {
        if (!Done_) {                      // pipeline breaker
            TRowSet batch{};
            while (Input_->Next(batch)) { updateDispatch(batch, &Ht_); Release(&batch); }
            Done_ = true;
            out = BuildOutputRowSet(Ht_);  // C++ финализация: один rowset из групп
            return true;                   // (далее можно чанковать на несколько Next)
        }
        return false;
    }
};
```
Выходной rowset аллоцируется как в `project_exec.cpp` (`Private`+`Destroy`),
колонки — из буферов таблицы/скопированные.

`exec/planner.cpp::Build`: ветка `TMaybeOp<TAggregateOperator>` — построить вход,
взять физический (pruned) `inputType`, скомпилировать update-ядро через
`TKernelCompiler::CompileAggregate`, создать `TRuntimeAggregate`.

### 6. CMake / CLI / тесты

- `qdb/CMakeLists.txt`: добавить `ops/aggregate.cpp`, `exec/aggregate_exec.cpp`,
  `kernel/...` (если новые .cpp), `modules/qumirdb_runtime.cpp`.
- CLI (`bin/cli.cpp`) менять не нужно — план читается из sexp, `AnnotateTypes` +
  `ApplyColumnPruning` + `planner.Build` уже общие; `(rel aggregate ...)`
  поддержан через парсер из п.2.
- Тесты:
  - `test/test_sexp.cpp`: roundtrip печати/разбора `(rel aggregate ...)`.
  - новый `test/test_aggregate.cpp`: на `TStubSource` (целочисленные колонки) —
    end-to-end count/sum/min/max с группировкой по 1 и 2 ключам; проверка
    значений по группам. Зарегистрировать в `test/CMakeLists.txt` (ctest).

---

## Stage 2 — намётки (строки и сложные данные)

- `str_view` тип + `str_hash` (FNV/xxhash) + побайтовое `=` — из
  `PLAN_OVERLOADS.md` (шаги 5–6). Generic Робин-Гуд параметризуется по строковому
  ключу через те же шаблоны.
- Хранение вариативных ключей: интернирование строк в арену (через `qdb_alloc`).
- Возможна финализация-обход HT в Qumir (второе ядро `agg_finalize`).

---

## Детальный порядок реализации

### A. Зафиксировать ограничения Stage 1

- [x] Один group key типа `i64`.
- [x] Аргументы агрегатов также только `i64` columns.
- [x] Без NULL/mask semantics.
- [x] Capacity — степень двойки; `capacity >= 4`.
- [x] Empty slot кодируется `Dist[index] == -1`; ключ `-1` остаётся допустимым.
- [x] Load factor для grow фиксируется константой, сначала `3/4`.
- [x] Все индексы, distances, sizes и slot ids имеют тип `i64`.

Результат шага: эти ограничения записаны в README и тестовых именах; код не
делает вид, что уже поддерживает остальные integer types.

### B. Standalone test harness

- [x] Добавить `test_aggregation` в CMake.
- [x] Передавать каталог kernels первым аргументом `test_aggregation` из CMake,
   как `test_reg` в qumir; `main` сохраняет его до `InitGoogleTest`.
- [x] Реализовать `ReadKernel(name)` относительно переданного каталога, без
   compile-time `QDB_SOURCE_DIR`.
- [x] Реализовать helper `CompileKernel(source)` с
   `CoreInput=true`, `ResolveCoreInput=true`, `AllowOverloads=true`.
- [x] Добавить минимальный concrete entry point `i64 -> i64` и проверить корректный
   вызов JIT function pointer.
- [x] Добавить диагностический ASSERT, который всегда выводит полную ошибку parser/
   resolver/lowering/codegen.

Результат шага: `ctest -R test_aggregation` компилирует `.oz` с диска и вызывает
entry point, не затрагивая logical/physical pipeline.

### C. Capability spikes для generic Key

Каждый пункт — отдельный маленький `.oz` и отдельный GTest. К следующему пункту
не переходим, пока текущий не компилируется и не выполняется.

- [x] Generic scalar parameter: identity для `<named Key ...>`, инстанс `i64`.
- [x] Overload dispatch: generic helper вызывает `rh_hash(Key)`, доступен только
   overload `rh_hash(i64)`.
- [x] Equality dispatch: generic helper вызывает `rh_key_equal(Key, Key)`.
- [x] Pointer instantiation: `<ptr Key>`, чтение `(index keys i)` для `Key=i64`.
- [x] Pointer write: запись `Key` в `<ptr Key>` через array assignment.
- [x] Generic carried-key swap при Robin Hood displacement.
- [ ] Передача generic helper-ов друг другу, чтобы проверить transitive template
   instantiation на реальной сигнатуре будущей таблицы.
- [x] Разобраться с обнаруженным поведением pointer write: причиной был выбор
  другой generic specialization через `Functions.back()`, сама pointer write
  работает и покрыта concrete/generic mutation тестами.
- [x] Не менять runner в `externals/qumir`: standalone kernels подстраиваются под
  существующий выбор `Module.Functions.back()`. До отдельного согласованного API
  вызываемый wrapper должен быть concrete и стоять последним.

Результат шага: доказано, что выбранное представление можно выразить в core-lang;
если конкретная форма не поддерживается, меняется ABI helper-а, а не весь hash
алгоритм.

### D. Контракт ключа `i64`

- [x] `rh_hash(i64) -> i64`: deterministic xorshift64* mixer.
- [x] Тестовые vectors: `0`, `1`, `-1`, `INT64_MIN`, `INT64_MAX`.
- [x] `rh_key_equal(i64, i64) -> bool` с positive/negative cases.
- [x] `rh_home(hash, capacity) -> i64`; проверить диапазон и power-of-two mask.
- [x] Явно проверить wrap-around индекса `capacity - 1 -> 0`.

На этом шаге нет таблицы и аллокаций.

### E. Read-only probing на заранее заполненных буферах

- [x] Сигнатура helper-а принимает `Keys`, `Dist`, `Capacity`, `QueryKey`.
- [x] Реализовать вычисление текущего probe distance.
- [x] Lookup в полностью пустой таблице.
- [x] Lookup ключа в home slot.
- [x] Lookup после одной и нескольких коллизий.
- [x] Lookup с wrap-around через конец массива.
- [x] Robin-Hood early exit: остановка, когда resident distance меньше query
   distance.
- [x] Жёсткий предел `probes <= capacity`, исключающий бесконечный цикл.

C++ в этих тестах только заполняет входные массивы и проверяет возвращённый
индекс; probing выполняется oz-lang кодом.

### F. Fixed-capacity insertion без grow

- [x] Вставка в пустой home slot.
- [x] Повторная вставка того же ключа возвращает существующий slot и не меняет таблицу.
- [x] Вставка с коллизией без displacement.
- [x] Один Robin-Hood swap.
- [x] Цепочка из двух и более swaps.
- [x] Swap-chain с wrap-around.
- [x] Заполненная таблица возвращает явный failure sentinel, не зацикливается.
- [x] После каждой операции отдельный invariant-check entry point проверяет:
   `Dist >= 0` для занятых slots, корректный home/distance и достижимость ключа
   через lookup.

### G. Stable dense SlotId

- [x] Добавить `SlotId[capacity]` и monotonic `Size`.
- [x] Новый key получает `slotId = old Size`.
- [x] При Robin-Hood swaps `SlotId` перемещается вместе с key.
- [x] Повторная вставка возвращает прежний slot id.
- [x] Проверить, что dense ids образуют диапазон `[0, Size)` без пропусков.

Это отделяет корректность hash table от aggregate buffers.

### H. Grow и rehash на oz-lang

- [x] Подключить `qdb_alloc/qdb_free`; сначала без `realloc`.
- [x] Oz-функция инициализации выделяет Keys/Dist/SlotId и заполняет Dist значением
   `-1`.
- [x] Oz-функция destroy освобождает все принадлежащие таблице буферы.
- [x] Rehash переносит entries в таблицу удвоенной capacity тем же insertion helper.
- [x] Stable SlotId сохраняется после rehash.
- [x] Grow запускается перед insert при достижении load factor `3/4`.
- [x] Тесты последовательных grow: `4 -> 8 -> 16 -> 32`.
- [x] После каждого grow проверяются все keys, Size и invariants.
- [x] Добавить проверки overflow размера allocation до умножения/удвоения.
- [x] При `qdb_alloc == nullptr` освобождать частичные allocations и возвращать
  failure, не изменяя старую таблицу. Детерминированный OOM hook не добавляем.

На standalone-этапе `qdb_alloc/qdb_free` остаются временным механизмом. Managed
allocator и его ABI сейчас не проектируем.

- [ ] Перед интеграцией добавить непрозрачный nullable query context и механически
  передать его через lifecycle/grow/aggregation functions; сначала тесты вызывают
  ядра с `nullptr`, и поведение allocation не меняется.
- [ ] Позже отдельно определить ownership и allocator API внутри query context,
  когда станет понятен lifecycle runtime query/execution instance.

### I. Агрегатные буферы, всё ещё без pipeline

- [x] `count(*)`: один `i64` buffer, zero-init нового dense slot; buffer и
  `GroupKeys` сохраняются по stable SlotId через grow.
- [x] `sum(i64)`: отдельная Oz reducer function обновляет существующую и новую
  группу через контракт `(prev, value, is_new) -> state`.
- [x] `min(i64)` и `max(i64)`: отдельные reducers используют `is_new` для
  first-value initialization без sentinel; проверены `INT64_MIN/MAX`.
- [x] Несколько агрегатов одновременно: standalone `count(*) + sum(i64)` хранит
  два buffers, индексируемых одним stable SlotId и переносимых через grow.
- [x] Обработка массива input keys/values одним oz entry point; `length=0`
  допускает null pointers, отрицательная длина отклоняется до чтения input.
- [x] Отдельный Oz finalize entry point пишет dense group keys и aggregate values
  в заранее предоставленные output buffers; недостаточная capacity отклоняется
  до записи.
- [x] Тесты пустого input, одной группы, всех уникальных keys и повторяющихся keys.

### J. Generic key dispatch и альтернативные представления ключа

- [x] Вынести generic probing/insertion/rehash с параметром
  `<named Key (template readable mutable)>`; внутри generic-кода не выполнять
  операций над представлением ключа напрямую. В standalone pair-тесте
  lookup+insert объединены в одну specialization из-за выбора последней lowered
  function текущим runner; при AST integration их можно снова разделить.
- [x] Оставить `rh_hash(Key)` и `rh_key_equal(Key, Key)` overload points;
  специализация generic-функций должна выбирать concrete overload по `Key`.
- [x] Ввести concrete key representation для пары `{i64, i64}` и добавить только
  hash/equality/copy overloads, не меняя generic Robin Hood functions.
- [x] Повторить collision, duplicate, rehash `8 -> 16` и count/sum aggregation
  tests для `{i64, i64}` с сохранением stable SlotId.
- [ ] Ввести concrete key representation для `{i64, f64}` с тем же набором
  overloads и повторить основные tests.
- [ ] Добавить standalone `f64` key только после фиксации контракта: `-0` и `+0`
  равны и имеют одинаковый hash; NaN либо запрещён на входе, либо
  канонизируется с согласованными equality/hash semantics.
- [x] Проверить compile-time dispatch отдельными `.oz` файлами, где разные key types
  дают заведомо разные overload implementations, но используют один и тот же
  generic `rh_*` код.

### K. Финальный standalone stress-test

Финальная точка этого этапа не использует logical/physical pipeline. C++ создаёт
только input arrays и получает C-compatible view результата:

```cpp
struct TAggregationResultView {
    int64_t* Keys;
    int64_t* Values;
    int64_t Capacity;
    int64_t Size;
};
```

> Примечание: к моменту реализации этого раздела разделы A-I уже определили
> финальный ABI (`count.oz`/`finalize.oz`/`check_invariants.oz`, dense
> `GroupKeys` + `AggBuffers` count/sum/min/max), который заменяет упрощённый
> `TAggregationResultView` выше. Stress-test ниже использует именно этот ABI;
> отдельный новый `.oz` entry point не понадобился.

- [x] Oz entry point принимает массивы input keys/values и их длину
  (`aggregation_count` op=1 `aggregate_batch`).
- [x] Таблица стартует с capacity `4` и сама выполняет grow `4 -> 8 -> 16 -> 32`
  (и далее, см. ниже).
- [x] Вход содержит collisions, duplicates, negative keys и ключи, вызывающие
  wrap-around.
- [x] Для duplicate keys значение обновляется (`Values[slot] += inputValue`) —
  реализовано через `agg_sum_i64_step`/`AggBuffers[1]` (а также count/min/max).
- [x] Entry point возвращает/заполняет `TAggregationResultView` с dense Keys и
  Values после всех grow — реализовано через `aggregation_finalize` (dense
  `GroupKeys` + `AggBuffers`).
- [x] C++ тест печатает таблицу при failure и проверяет все `(key, sum)` без
  зависимости от порядка slots.
- [x] Отдельный oz invariant checker подтверждает Size, probe distances,
  достижимость каждого key и уникальность dense SlotId.
- [x] Oz destroy освобождает все allocation после проверки view.
- [x] Stress-вариант прогоняет не менее 1000 deterministic input rows и вызывает
  несколько последовательных grow.

Реализация: `TEST(AggregationKernel, OzStressAggregationHandlesLargeDeterministicInput)`
в `test/test_aggregation.cpp` — 2000 строк по детерминированному LCG, ключи в
`[-150, 150]` (collisions/duplicates/negative/wrap-around), 3 батча через op=1,
capacity растёт `4 -> ... -> >=256`, проверки `Size`/invariants/уникальность
`SlotId` через `check_invariants.oz`, сравнение с `std::unordered_map`-референсом
по count/sum/min/max через `aggregation_finalize`, затем op=6 destroy.

### L. Интеграция с qdb

Как и в A-K, каждый подраздел — небольшой шаг со своим тестом; к следующему не
переходим, пока текущий не собирается и не проходит. `CompileKernelAst`
возвращает только `Module.Functions.back()` (один указатель), поэтому per-query
update-кernel остаётся ОДНОЙ функцией с `op`-диспетчеризацией (как
`aggregation_count`), а finalize компилируется отдельным вызовом как
неизменный `aggregation_finalize` из `finalize.oz`.

**Ограничения Stage 1 pipeline integration** (сужают standalone-ядро,
проверяются тестами, расширения — отдельные шаги после стабилизации):
- Один group key типа `i64`.
- Все `agg`, кроме `count(*)`, ссылаются на одну и ту же `i64`-колонку-аргумент.
- `agg.Func` ∈ {count, sum, min, max}.
- Если ни один `agg` не имеет `Arg` (только `count(*)`), generated kernel
  передаёт в `count_update` константу `0` вместо значения колонки.

#### L1. Библиотека: инъекция готовых .oz функций в один AST-блок

- [x] Хелпер парсит `count.oz` core-парсером и возвращает его `FunDecl`'ы (без
  `aggregate_batch`/`aggregation_count` — они не нужны и не должны стать entry
  point).
- [x] Хелпер `MergeKernelLibrary(libraryFunDecls, generatedEntry) -> TBlockExpr`
  собирает один блок, где `generatedEntry` — последний `FunDecl`.
- [x] Тест: тривиальный generated entry (например, обёртка над
  `count_init`+`count_destroy` с фиксированной capacity) компилируется через
  `CompileKernelAst` с `AllowOverloads=true` и выполняется.

Результат шага: подтверждён механизм "проверенные .oz функции + сгенерированная
обёртка в одном AST", без TRowSet/HashTable семантики агрегации.

> Реализация: `qdb/kernel/lib.h`/`lib.cpp` — `ParseFunctionLibrary(source, exclude)`
> парсит произвольный `(block (fun ...) ...)` и возвращает `Stmts`, отфильтрованные
> по имени `FunDecl`; `MergeKernelLibrary(library, entry)` строит новый `TBlockExpr`
> с `entry` последним. Тест `OzMergedLibraryCompilesGeneratedEntryPoint`
> (`test/test_aggregation.cpp`) парсит `count.oz` без `aggregate_batch`/
> `aggregation_count`, добавляет сгенерированный `lib_smoke(ht, capacity, op)`
> (op=0 → `count_init`, иначе → `count_destroy`), компилирует merged-блок через
> `CompileKernelAst` (с `AllowOverloads=true`, без `ResolveCoreInput` — он не
> используется этим методом) и проверяет инициализацию/уничтожение `HashTable`.

#### L2. GenAggregateKernelAst — update kernel над TRowSet

- [x] `GenAggregateKernelAst(inputType, fieldIndices, keyField,
  std::optional<argField>) -> TExprPtr` строит одну функцию
  `agg_dispatch(ref HashTable ht, ref TRowSet batch, i64 arg, i64 op) -> i64`:
  - извлекает `cols[keyIdx].Data` и (если есть) `cols[argIdx].Data` как
    `<ptr i64>`, переиспользуя паттерн столбцовых указателей из
    `GenFilterKernelAst`;
  - `op == 0`: `return cast(count_init(ht, arg), i64)` (init, capacity=arg);
  - `op == 1`: цикл `i in [0, n)`, пропуская строки с `selection[i] == 0` (если
    `selection != null`), вызывает `count_update(ht, keys[i], value)`, где
    `value = values[i]` либо константа `0`, если arg column отсутствует;
  - `op == 6`: `count_destroy(ht)`.
- [x] Проверить, допускает ли `<ref TRowSet> batch` пустое/dummy значение при
  `op` 0/6 (по аналогии с `<ptr i64> = nullptr` у `aggregation_count`); если
  нет — передавать валидный, но неиспользуемый `TRowSet{}`.
- [x] Через L1 объединяется с `count.oz`.
- [x] Тест без planner/exec: собрать `TRowSet` с 2 i64-колонками (key, value) и
  `Selection`, вызвать `agg_dispatch` op=0/1 (несколько батчей)/6, проверить
  состояние `HashTable` (как в стресс-тесте K, но вход — `TRowSet`+`Selection`,
  а не raw массивы).
- [x] Отдельный тест: `count(*)` без arg column (value-константа `0`).

Результат шага: per-query generated update kernel строит ту же hash table, что
и `count.oz` напрямую, читая данные из `TRowSet`.

> Реализация: `qdb/kernel/gen.{h,cpp}` — `GenAggregateKernelAst(fieldIndices,
> keyField, argField, columnType, rowSetType, hashTableType) -> TExprPtr`
> строит единственный `FunDecl` `agg_dispatch(ref HashTable ht, ref TRowSet
> batch, i64 arg, i64 op) -> i64` с вложенным `if/elif/else` по `op` (0 → init,
> 1 → update batch, иначе → destroy), полностью через C++ AST-конструкторы (без
> текстового шаблона), по образцу `GenFilterKernelAst`.
>
> Отклонения от исходной формулировки:
> - Параметр `inputType` убран из сигнатуры — он не используется (Stage 1
>   жёстко приводит key/arg колонки к `i64` независимо от объявленных типов
>   `inputType.Fields`).
> - **Критичный момент типов**: параметры `ht`/`batch` должны иметь тип
>   `<ref HashTable>`/`<ref TRowSet>` ТОЧНО в том виде, в каком его получает
>   резолвер для текстового `<ref HashTable>` в `count.oz`, т.е.
>   `TReferenceType(TNamedType("HashTable", hashTableType))`, а НЕ
>   `TReferenceType(hashTableType)` напрямую (raw struct). Резолвер превращает
>   текстовую аннотацию `<ref HashTable>` в `TReferenceType(TNamedType("HashTable",
>   UnderlyingType=hashTableType))`; `EqualTypes` для `TNamedType` сравнивает по
>   `Name`, а для raw `TStructType` (`TypeName()=="Struct"`) — у `"Struct" !=
>   "Named"` сразу `false`. Без этой обёртки вызов `count_init(ht, arg)` падал с
>   `Аргумент #1: тип 'Ref' не совпадает с типом ссылочного параметра 'Ref'`.
>   `batch` обёрнут аналогично (`TNamedType("TRowSet", rowSetType)`) для
>   консистентности, хотя `<ref TRowSet>` нигде не передаётся в `count.oz`.
> - `<ref TRowSet> batch` при `op` 0/6: `<ref T>` реализован как raw pointer;
>   поля `batch` читаются только внутри ветки `op==1` (codegen генерирует
>   условные `br`, и при `op != 1` инструкции этой ветки не выполняются), поэтому
>   тесты передают `nullptr` как `batch` для op=0/6 — безопасно подтверждено.
> - Результат `count_update(ht, key, value)` (dense slot или -1) не используется
>   в L2, но чтобы не вызывать функцию как bare-statement-выражение, заведена
>   переменная `dense_slot` и используется `(= dense_slot (call count_update ...))`.
>
> Тесты (`test/test_aggregation.cpp`, через helper `CompileAggregateDispatch`,
> который генерирует `agg_dispatch`, объединяет его с `count.oz` минус
> `aggregate_batch`/`aggregation_count` через L1 и компилирует
> `CompileKernelAst` с `AllowOverloads=true`):
> - `OzAggregateDispatchUpdatesHashTableFromRowSet`: 2 i64-колонки (`k`, `v`),
>   `op=0` init(capacity=4), затем 2 батча через `op=1` — первый с `Selection`
>   (фильтрует одну строку), второй без `Selection` — сверка `Dist`/`Keys`/
>   `SlotId`/`AggBuffers[0..3]` с эталонной `std::unordered_map`, затем `op=6`
>   destroy.
> - `OzAggregateDispatchCountStarWithoutArgColumn`: одна i64-колонка `k`,
>   `argField = std::nullopt` — проверяет `count(*)`: `AggBuffers[0]` = счётчик,
>   `AggBuffers[1..3]` (sum/min/max) == 0 для всех групп (константа `0` вместо
>   значения колонки).
>
> Build + полный `ctest` (3/3) прошли.

> **Статус L2 — промежуточный, заменяется в L2b/L2c.** `agg_dispatch` выше
> вызывает `count_init`/`count_update`/`count_destroy` из `count.oz`, которые
> жёстко содержат `NumAggs = 4` и безусловно вызывают четыре фиксированные
> reducer-функции (`agg_count_step`/`agg_sum_i64_step`/`agg_min_i64_step`/
> `agg_max_i64_step`); `aggregation_count`'s `op` 2-5 выбирает, какой из четырёх
> буферов прочитать. Это — предопределённый набор count/sum/min/max с
> синтаксическим сахаром и `op`-код как механизм ВЫБОРА агрегатной функции —
> костыль, оправданный только для интерпретации узлов, а не для AST-инъекции.
> Конечная цель (см. §4, "reduce(prev, value, is_new) -> state") — кастомные
> reduce-функции с предопределёнными именами/сигнатурой, генерируемые и
> вставляемые в AST под запрос (`N = aggs.size()`, не фиксированные 4), вызов —
> статическими прямыми именованными вызовами, без function pointers и без
> `op`-кода для выбора агрегатной функции. L2a/L2b/L2c ниже заменяют
> `agg_dispatch` + `count.oz`'s fixed-4 машинерию на этой основе; код и тесты
> текущего L2 остаются как промежуточный, проходящий шаг до их завершения.

#### L2a. GenReducerFunDecls — кастомные reduce-функции с предопределёнными именами

- [x] `GenReducerFunDecls(funcs: vector<string>) -> vector<FunDecl>`
  (`qdb/kernel/gen.{h,cpp}`) генерирует `N = funcs.size()` функций
  `reduce_0 .. reduce_{N-1}` с контрактом из §4
  `(i64 prev, i64 value, bool is_new) -> i64`; тело `reduce_i` зависит от
  `funcs[i] ∈ {"count","sum","min","max"}` и совпадает с
  `agg_count_step`/`agg_sum_i64_step`/`agg_min_i64_step`/`agg_max_i64_step`
  (`count.oz`, строки 13-27), но под позиционным именем. `N` произвольно
  (не фиксировано на 4), порядок/состав = `aggs` запроса.
- [x] Тест без HashTable/TRowSet/`count.oz`: сгенерировать `reduce_0..reduce_{N-1}`
  и тестовую "smoke"-обёртку, которая для каждого `i` делает
  `out[i] = reduce_i(prev, value, is_new)` (unroll, статические именованные
  вызовы — образец будущего `agg_update`), объединить через L1
  (`MergeKernelLibrary`), скомпилировать и сравнить `out[]` с ожидаемыми
  count/sum/min/max значениями. Покрыть `N=4` (count/sum/min/max, тот же
  порядок, что в `count.oz`), `N=2` в нестандартном порядке (`max,count`) и
  `N=1` (`sum`) — подтверждает, что число и состав сгенерированных функций
  равны `funcs`, а не фиксированы.

> Реализация: `qdb/kernel/gen.{h,cpp}` — `GenReducerFunDecls(funcs) ->
> vector<TExprPtr>`, через те же AST-helper'ы (`ident`/`numI64`/`binary`/
> `TIfExpr`/`TReturnExpr`/`TFunDecl`), что и `GenAggregateKernelAst`.
> Неизвестный `funcs[i]` — программная ошибка (логический оператор уже
> валидирует `Func ∈ {count,sum,min,max}`), поэтому `GenReducerFunDecls`
> бросает `std::invalid_argument` без отдельного `TError`-контракта.
>
> Тест `OzGeneratedReducersAreCalledByStaticName` (`test/test_aggregation.cpp`)
> — новые helper'ы `GenReduceSmokeEntry(numReducers)` (генерирует
> `reduce_smoke(i64 prev, i64 value, i64 is_new_flag, <ptr i64> out)`, где
> `is_new = is_new_flag != 0`, и для каждого `i` пишет
> `out[i] = reduce_i(prev, value, is_new)`) и `CompileReducerSmoke(funcs, ...)`
> (генерирует `reduce_0..reduce_{N-1}` + `reduce_smoke`, объединяет через L1,
> компилирует `CompileKernelAst` с `AllowOverloads=true`, без регистрации
> `QumirDbModule` — не нужен для чистой арифметики). Проверены `{count, sum,
> min, max}` (N=4), `{max, count}` (N=2), `{sum}` (N=1) на нескольких
> `(prev, value, is_new)`.
>
> Build + полный `ctest` (3/3) прошли.

#### L2b. NumAggs-generic table codegen, вызывающий reduce_0..reduce_{N-1}

> Уточнение разбивки (после L2a): `AggBuffers`/`NumAggs` в `HashTable` уже
> структурно generic (`<ptr <ptr i64>>` + `i64`). Поэтому alloc/free/
> zero-init/copy-при-rehash для `N` буферов можно написать ОДИН РАЗ как
> обычный `while (i < ht.NumAggs)`-цикл по `AggBuffers[i]` — это код, не
> зависящий от `N`, и идёт в shared library (как `rh_hash_i64`/
> `count_lookup`/`count_insert_existing`). Единственное место, где число и
> состав reducer'ов должны быть известны статически — это вызовы
> `reduce_i(prev, value, is_new)` для каждого буфера `i` (oz не поддерживает
> вызовы по указателю). Поэтому L2b разбит на:
>
> - **L2b-1** — `GenApplyReducersFunDecl(N)`: единственная per-query
>   генерируемая функция, которая делает unroll по `i in [0,N)`.
> - **L2b-2** — фиксированная (НЕ генерируемая) `NumAggs`-generic shared
>   library `agg_init`/`agg_rehash`/`agg_update`/`agg_destroy`/`agg_finalize`,
>   вызывающая `agg_apply_reducers` по имени.

- [x] **L2b-1**: `GenApplyReducersFunDecl(numReducers: size_t) -> FunDecl`
  (`qdb/kernel/gen.{h,cpp}`) генерирует
  `agg_apply_reducers(<ptr <ptr i64>> agg_buffers, i64 dense_slot, i64 value,
  bool is_new)`, которая для `i in [0, numReducers)` делает
  `agg_buffers[i][dense_slot] = reduce_i(agg_buffers[i][dense_slot], value,
  is_new)` — `N` статических прямых именованных вызовов в
  `reduce_0..reduce_{N-1}` (L2a). Это единственный кусок кода, генерируемый
  заново под каждый запрос для L2b.
- [x] **L2b-2**: переписать `count_init`/`count_rehash`/`count_update`/
  `count_destroy` (`count.oz`) и `aggregation_finalize` (`finalize.oz`) как
  `agg_init`/`agg_rehash`/`agg_update`/`agg_destroy`/`agg_finalize` —
  ФИКСИРОВАННЫЙ `NumAggs`-generic код (НЕ генерируется заново для каждого
  `N`): вместо 4 именованных буферов (`counts/sums/mins/maxs`) — цикл по
  `AggBuffers[0..NumAggs-1]` (`qdb_alloc`/`qdb_free`/zero-init/copy-при-rehash);
  `agg_update` вызывает `agg_apply_reducers` (L2b-1, по имени) в двух точках
  (обновление существующего слота и инициализация нового). `agg_init`
  принимает `num_aggs` доп. параметром (вызывающий код передаёт `N = aggs.size()`).
  `agg_finalize` принимает `<ptr <ptr i64>> output_buffers` (вместо 4
  отдельных `output_counts/sums/mins/maxs`) и копирует `AggBuffers[0..NumAggs-1]`
  в `output_buffers[0..NumAggs-1]` тем же generic-циклом.
- [x] Agg-агностичная Robin Hood probing/insertion (`rh_hash_i64`,
  `count_lookup`, `count_insert_existing` — не трогают `AggBuffers`/reducers)
  остаётся shared library, переиспользуемой через L1 без изменений.
- [x] Тесты по образцу существующих H/I/K, но для `N != 4` (например, один
  `count(*)` или `{sum, max}`), подтверждающие, что `agg_init`/`agg_update`/
  `agg_rehash`/`agg_destroy`/`agg_finalize` (L2b-2) + `agg_apply_reducers`
  (L2b-1, сгенерированный под конкретный `N`) корректны для произвольного `N`.

> Реализация L2b-1: `qdb/kernel/gen.{h,cpp}` —
> `GenApplyReducersFunDecl(numReducers) -> TExprPtr` (`TFunDecl`), теми же
> AST-helper'ами (`ident`/`numI64`/`TIndexExpr`/`TArrayAssignExpr`/
> `TCallExpr`), что и `GenReducerFunDecls`. Тест
> `OzApplyReducersUpdatesAllBuffersByStaticName` (`test/test_aggregation.cpp`)
> — новые helper'ы `GenApplyReducersSmokeEntry()` (генерирует
> `apply_smoke(<ptr <ptr i64>> agg_buffers, i64 dense_slot, i64 value,
> i64 is_new_flag)`, переводит `is_new_flag` в `bool` и зовёт
> `agg_apply_reducers`) и `CompileApplyReducersSmoke(funcs, ...)` (генерирует
> `reduce_0..reduce_{N-1}` + `agg_apply_reducers` + `apply_smoke`, объединяет
> через L1). Проверены `{count, sum, min, max}` (N=4, `dense_slot=0`,
> несколько вызовов подряд), `{max, count}` (N=2, нестандартный
> порядок/состав, `dense_slot=1` — проверяет индексацию и то, что соседний
> слот не затрагивается) и `{sum}` (N=1). Без HashTable/TRowSet/`count.oz`.
>
> Build + полный `ctest` (3/3) прошли.

> Реализация L2b-2: `qdb/kernel/aggregation/count.oz` — добавлены (рядом со
> старыми `count_init`/`count_rehash`/`count_update`/`count_destroy`, которые
> НЕ тронуты) `agg_init(ht, capacity, num_aggs) -> bool`,
> `agg_destroy(ht)`, `agg_rehash(ht, new_capacity) -> bool`,
> `agg_update(ht, key, value) -> i64` — все построены вокруг
> `while (a < ht.NumAggs)`-циклов по `AggBuffers[a]`; `agg_update` зовёт
> внешний (per-query, L2b-1) `agg_apply_reducers` в двух точках (существующий
> слот и новый слот после `count_insert_existing`). `rh_hash_i64`/
> `count_lookup`/`count_insert_existing` не изменены и используются и старым,
> и новым кодом. `qdb/kernel/aggregation/finalize.oz` — добавлен (рядом со
> старым `aggregation_finalize`) `agg_finalize(ht, output_keys,
> output_buffers, output_capacity) -> i64`, копирующий `GroupKeys` и
> `AggBuffers[0..NumAggs-1]` в `output_buffers[0..NumAggs-1]` тем же
> generic-циклом.
>
> `test/test_aggregation.cpp` — новый helper `CompileAggTableSmoke(funcs, ...)`
> парсит `count.oz` (исключая `agg_count_step/.../count_init/.../aggregate_batch/
> aggregation_count`, т.е. оставляя `rh_hash_i64/count_lookup/
> count_insert_existing/agg_init/agg_destroy/agg_rehash/agg_update`) и
> `finalize.oz` (оставляя только `agg_finalize`), генерирует
> `reduce_0..reduce_{N-1}` (L2a) + `agg_apply_reducers` (L2b-1) для `funcs`,
> и собирает всё через L1 в `agg_table_smoke(ht, key, value, output_keys,
> output_buffers, output_capacity, capacity_arg, num_aggs, op)` —
> op-диспетчер 0=init/1=update/2=finalize/3=destroy (тот же паттерн, что
> `aggregation_count`/`lib_smoke`). Важно: в объединённом блоке
> `reduce_0..reduce_{N-1}` и `agg_apply_reducers` должны идти ПЕРЕД
> `agg_update` — аннотация типов идёт одним проходом по `Stmts` в порядке
> объявления, и вызываемая функция должна быть уже аннотирована к моменту
> аннотации тела вызывающей.
>
> Новые тесты: `OzGenericAggTableHandlesNonDefaultAggregateSet` (`N=2`,
> `{sum, max}` — нестандартный набор/порядок относительно `count.oz`'s
> `{count,sum,min,max}`, с ростом таблицы `2 -> 4 -> 8`) и
> `OzGenericAggTableHandlesSingleCountAggregate` (`N=1`, `{count}`, рост
> `2 -> 4`) — обе проверяют init/update/finalize/destroy через
> `agg_table_smoke`.
>
> Старые `count_init/count_rehash/count_update/count_destroy/
> aggregate_batch/aggregation_count` и `aggregation_finalize` НЕ тронуты —
> добавлен `kAggTableGenericFuncs`/`kAggFinalizeGenericFuncs` exclude-набор
> в `test_aggregation.cpp`, чтобы кернелы, не предоставляющие
> `agg_apply_reducers` (т.е. без сгенерированного L2b-1), не включали новые
> `agg_init/agg_rehash/agg_update/agg_destroy`/`agg_finalize` и продолжали
> компилироваться как раньше.
>
> Build + полный `ctest` (3/3, включая 28/28 в `test_aggregation`) прошли.

Результат шага: таблица и финализация больше не привязаны к набору
count/sum/min/max — `N` и состав reducer'ов полностью определяются запросом.

#### L2c. Перешить agg_dispatch на сгенерированные agg_init/agg_update/agg_destroy

- [x] `GenAggregateKernelAst` (L2) меняет вызовы `count_init`/`count_update`/
  `count_destroy` на сгенерированные в L2b `agg_init`/`agg_update`/
  `agg_destroy` (та же `op`-диспетчеризация init/update/destroy — это про
  lifecycle/единственный entry point у `CompileKernelAst`, а не про выбор
  агрегатной функции, и остаётся).
- [x] Существующие тесты L2 (`OzAggregateDispatchUpdatesHashTableFromRowSet`,
  `OzAggregateDispatchCountStarWithoutArgColumn`) адаптируются/дополняются для
  произвольного `N`/состава `aggs`.

Результат шага: per-query update kernel целиком построен из L2a (reducers) +
L2b (NumAggs-generic table) + L2c (TRowSet-обёртка), без фиксированного
набора count/sum/min/max и без `op`-кода для выбора агрегатной функции.

Реализация L2c:

- `GenAggregateKernelAst` (`qdb/kernel/gen.{h,cpp}`) получил новый параметр
  `size_t numAggs`. Изменения тела:
  - `op == 0`: `count_init(ht, arg)` → `agg_init(ht, arg, numAggs)`
    (третий аргумент — число агрегатных буферов, известное статически на
    этапе генерации запроса).
  - `op == 1`, обработка строки: `count_update(ht, key, value)` →
    `agg_update(ht, key, value)` (сигнатура не изменилась).
  - иначе (destroy): `count_destroy(ht)` → `agg_destroy(ht)`.
  - имя самой функции (`agg_dispatch`) и форма `op`-диспетчеризации
    init/update/destroy не изменились — это lifecycle-диспетчеризация
    единственного entry point, а не выбор агрегатной функции.
- `agg_init`/`agg_update`/`agg_destroy` сами вызывают `agg_apply_reducers`
  (L2b-1) → `reduce_0..reduce_{numAggs-1}` (L2a), поэтому результирующий
  `agg_dispatch` должен компилироваться смержённым с этими сгенерированными
  функциями ДО merge с NumAggs-generic подмножеством `count.oz`
  (`agg_init`/`agg_rehash`/`agg_update`/`agg_destroy` +
  `rh_hash_i64`/`count_lookup`/`count_insert_existing`), и именно в этом
  порядке (reducers/agg_apply_reducers раньше agg_update в `Stmts`) — та же
  ordering-constraint, что и в L2b-2.
- `test/test_aggregation.cpp`:
  - Новая общая константа `kCountOzFixedFuncs` — exclude-набор для
    `count.oz`, отбрасывающий фиксированный `NumAggs=4`-путь
    (`agg_count_step`/`agg_sum_i64_step`/`agg_min_i64_step`/
    `agg_max_i64_step`/`count_init`/`count_rehash`/`count_update`/
    `count_destroy`/`aggregate_batch`/`aggregation_count`) и оставляющий
    shared-хелперы + NumAggs-generic `agg_*`. Используется и в
    `CompileAggTableSmoke` (L2b-2, ранее — инлайновый список), и в новой
    `CompileAggregateDispatch`.
  - `CompileAggregateDispatch` получил параметр
    `const std::vector<std::string>& funcs`: строит
    `GenReducerFunDecls(funcs)` + `GenApplyReducersFunDecl(funcs.size())` +
    `ParseFunctionLibrary(count.oz, kCountOzFixedFuncs)` (в этом порядке —
    ordering-constraint), затем
    `GenAggregateKernelAst(..., numAggs=funcs.size(), ...)` как entry.
  - `OzAggregateDispatchUpdatesHashTableFromRowSet` и
    `OzAggregateDispatchCountStarWithoutArgColumn` теперь передают
    `funcs = {"count","sum","min","max"}` (N=4, регрессионная проверка —
    результаты не изменились после перешивки на `agg_*`).
  - Новый тест `OzAggregateDispatchHandlesNonDefaultAggregateSet`: N=2,
    `funcs = {"sum","max"}`, через `agg_dispatch`/`TRowSet` проверяет, что
    `AggBuffers[0]/[1]` содержат именно sum/max по группам (без
    count/min-слотов) — подтверждает произвольность `N`/состава `aggs`.
  - Build + полный `ctest` (3/3, включая 29/29 в `test_aggregation`)
    прошли.

#### L3. CompileAggregate

- [x] `kernel/compiler.h`: `CompileAggregate(inputType, groupKeys, aggs) ->
  TAggregateKernels` — для данного `aggs` генерирует и компилирует L2a
  (`reduce_0..reduce_{N-1}`) + L2b (`agg_init`/`agg_rehash`/`agg_update`/
  `agg_destroy`/`agg_finalize` для `N=aggs.size()`) + L2c (`agg_dispatch` над
  `TRowSet`), всё через один merged AST/`CompileKernelAst`; `Finalize` — также
  генерируется в L2b (N-generic), а не фиксированный `aggregation_finalize`.
- [x] Валидация ограничений Stage 1 pipeline integration (см. выше) — понятная
  ошибка при нарушении, а не падение в codegen.
- [x] Тест компилятора на синтетическом `TStructType`: вызвать
  `Dispatch`+`Finalize` напрямую, без planner/exec.

Результат шага: один публичный API, инкапсулирующий L1+L2a+L2b+L2c и
финализацию.

Реализация L3:

- `TKernelCompiler::CompileAggregate` (`qdb/kernel/compiler.cpp`) сначала
  валидирует Stage 1 ограничения и при нарушении бросает `NQumir::TError` с
  понятным сообщением (а не падает в codegen):
  - ровно один `groupKeys` и его тип — `i64`;
  - каждый `agg.Func ∈ {count, sum, min, max}`;
  - каждый непустой `agg.Arg` — это `TIdentExpr`, ссылающийся на колонку, и
    все такие колонки совпадают (единственная общая `i64`-колонка-аргумент,
    как и зафиксировано в контракте `TAggregateSpec`).
- Dispatch собирается как `agg_dispatch` (L2c), смерженный (через
  `MergeKernelLibrary`) с `GenReducerFunDecls(funcs)` + `GenApplyReducersFunDecl`
  (L2a/L2b-1, под конкретный `funcs = {agg.Func...}`) и NumAggs-generic
  подмножеством `count.oz` (`kCountOzFixedFuncs` как exclude-список, L2b-2),
  в порядке reducers/`agg_apply_reducers` → `agg_init/agg_update/agg_destroy` →
  `agg_dispatch` (ordering-constraint: однопроходная типизация `Stmts`).
- Finalize собирается отдельно как `agg_finalize` (`finalize.oz`,
  NumAggs-generic, L2b-2) — он самодостаточен и не требует reducers/library.
- `CompileKernelAst` возвращает один entry point на модуль, поэтому Dispatch и
  Finalize компилируются ДВУМЯ независимыми `TLLVMRunner` (каждый со своим
  `RegisterModule(dbModule, true)`); оба runner'а живут в `shared_ptr`,
  захваченных лямбдами `TAggregateKernels::Dispatch`/`Finalize`.
- Новый тест `AggregationCompiler.CompileAggregateDispatchAndFinalize`
  (`test/test_aggregation.cpp`) строит синтетический `TStructType{k: i64,
  v: i64}` и `aggs = {count(*), sum(v), min(v), max(v)}` (N=4), вызывает
  `CompileAggregate`, затем напрямую `Dispatch` (init → update по `TRowSet` из
  6 строк/3 групп → destroy) и `Finalize`, и сравнивает результат с эталоном,
  посчитанным в C++ — без planner/exec.
- Build (`cmake --build build -j`) и полный `ctest` (3/3, включая 30/30 в
  `test_aggregation`, было 29/29) прошли.

#### L4. TRuntimeAggregate — exec node

- [x] `qdb/exec/aggregate_exec.{h,cpp}`: pipeline breaker. `Next()`: на первом
  вызове — `Dispatch(&ht, ..., initialCapacity, 0)`, затем
  `while (Input_->Next(batch)) { Dispatch(&ht, &batch, 0, 1); Release(&batch); }`,
  затем `Finalize` в свежевыделенные буферы, затем `Dispatch(&ht, ..., 0, 6)`
  (destroy HT — он больше не нужен после finalize).
- [x] `BuildOutputRowSet`: аллокация `TColumn`-буферов по образцу
  `project_exec.cpp` (`Private`+`Destroy`); колонки в порядке
  `GroupKeys() ++ Aggs()`, agg-колонка берётся из
  `AggBuffers[FuncToBufferIndex(agg.Func)]`; буферы, которые не запрошены
  (например `min`/`max`, если не используются), всё равно вычисляются, но не
  копируются в output (Stage 1 не оптимизирует это).
- [x] Тест: сценарий K (множественные батчи, grow, collisions) через
  `TRuntimeAggregate.Next()` на нескольких `TRowSet`, сравнить итоговый
  `TRowSet` с эталоном.

Результат шага: рантайм-узел самодостаточен и протестирован без planner.

Реализация L4:

- `TRuntimeAggregate` (`qdb/exec/aggregate_exec.{h,cpp}`) — pipeline breaker
  с флагом `Done_`: первый `Next()` выполняет полный жизненный цикл
  `TAggregateKernels` на стековом буфере `kHashTableSize`-байт (`THashTable`
  — локальная копия C-layout из `modules/qumirdb.cpp`, как в
  `test_aggregation.cpp`):
  - `Dispatch(ht, nullptr, kInitialCapacity=4, kOpInit=0)`;
  - `while (Input_->Next(batch)) { Dispatch(ht, &batch, 0, kOpUpdate=1);
    Release(&batch); }`;
  - читает `ht->Size`, аллоцирует `Keys[Size]` + `NumAggs` буферов
    `int64_t[Size]`, вызывает `Finalize(ht, keys, buffers, Size)`;
  - `Dispatch(ht, nullptr, 0, kOpDestroy=2)` (любое значение `op ∉ {0,1}`
    уходит в ветку destroy — см. `compiler.h`, "otherwise: destroy").
  Второй и последующие `Next()` возвращают `false`.
- `BuildOutputRowSet`-эквивалент — инлайн в `Next()`: выходные буферы лежат в
  `TAggregateRowSetData` (Private, `std::vector<int64_t>` × (1 + NumAggs)),
  колонки в порядке `output_keys ++ output_buffers[0..NumAggs-1]`. Поскольку
  `agg_finalize` (L2b-2, NumAggs-generic) уже отдаёт `AggBuffers[i]`
  позиционно по `aggs[i]` (а не через `FuncToBufferIndex`, который
  относился к старому фиксированному `count.oz`-пути), отдельного
  маппинга func→buffer не требуется — порядок выходных колонок `1:1`
  совпадает с `aggs` и с `ComputeAggregateOutputType`.
- Новый тест `AggregationExec.RuntimeAggregateMultiBatchGrowAndCollisions`
  (`test/test_aggregation.cpp`) строит `CompileAggregate` для
  `{count(*), sum(v), min(v), max(v)}`, кормит `TRuntimeAggregate` тремя
  батчами из 200 строк с 24 различными ключами в `[-12, 11]` (LCG, как в
  `OzStressAggregationHandlesLargeDeterministicInput`) — таблица проходит
  несколько grow `4 -> 8 -> 16 -> 32`; результат `Next()` сравнивается с
  `std::unordered_map`-эталоном по всем группам без зависимости от порядка
  строк, второй `Next()` возвращает `false`.
- `qdb/exec/aggregate_exec.cpp` зарегистрирован в `qdb/CMakeLists.txt`.
- Build (`cmake --build build -j`) и полный `ctest` (3/3, включая 31/31 в
  `test_aggregation`, было 30/30) прошли.

#### L5. Planner wiring

- [x] `exec/planner.cpp::Build`: ветка `TMaybeOp<TAggregateOperator>` —
  построить `input`, взять pruned `inputType`, вычислить `fieldIndices`,
  вызвать `CompileAggregate`, создать `TRuntimeAggregate`.
- [x] Тест: собрать logical plan вручную (`MakeAggregate` + `AnnotateTypes` +
  `ApplyColumnPruning`), прогнать через `Build` на `TStubSource`, сравнить
  результат с эталоном.

Реализация L5:

- `exec/planner.cpp::Build` — новая ветка `TMaybeOp<TAggregateOperator>`
  (после `TProjectOperator`, перед финальным `throw`): строит `input =
  Build(agg->Input())`, берёт pruned `inputType` (через
  `input->OutputType()`, как у Filter/Project), вызывает
  `compiler.CompileAggregate(*inputType, agg->GroupKeys(), agg->Aggs())`,
  считает `outputType = ComputeAggregateOutputType(input->OutputType(),
  agg->GroupKeys(), agg->Aggs())` (из physical, а не logical типа — логика
  идентична Filter/Project) и возвращает `TRuntimeAggregate`. Добавлены
  инклюды `exec/aggregate_exec.h` и `ops/aggregate.h`; обновлён doc-комментарий
  в `planner.h` ("TAggregateOperator → TRuntimeAggregate ✓").
- Новый тест `AggregationExec.PlannerBuildsAggregateOverSource`
  (`test/test_aggregation.cpp`) — локальный `TVectorSource : ISource` с
  схемой `{k: i64, v: i64}` и 2 батчами по 4 строки (8 строк, 4 группы,
  4-я группа появляется только во втором батче); строит
  `TSourceOperator -> TAggregateOperator{keys=[k], aggs=[count(*) as c,
  sum(v) as s, min(v) as mn, max(v) as mx]}`, прогоняет `AnnotateTypes` +
  `ApplyColumnPruning` + `TPhysicalPlanner::Build`, сравнивает результат
  `Next()` с `std::unordered_map`-эталоном (как в L4), проверяет, что второй
  `Next()` возвращает `false`.
- Build (`cmake --build build -j`) и полный `ctest` (3/3, включая 32/32 в
  `test_aggregation`, было 31/31) прошли.

#### L6. Sexp roundtrip + end-to-end

- [x] `test/test_sexp.cpp`: roundtrip печати/разбора `(rel aggregate ...)`
  (если ещё не покрыто тестами из разделов 2/3).
- [x] Новый `test/test_aggregate.cpp`: e2e —
  `(rel aggregate (rel source ...) (keys k) (agg c count) (agg s sum v) ...)`
  через sexp-парсинг -> `AnnotateTypes` -> `ApplyColumnPruning` ->
  `TPhysicalPlanner::Build` -> `Next()`, на `TStubSource` с несколькими
  группами (1 и >1 уникальных ключей), сравнение с ручным эталоном.
- [x] Зарегистрировать `test_aggregate` в `test/CMakeLists.txt` (ctest).

Реализация L6:

- Roundtrip печати/разбора `(rel aggregate ...)` уже был покрыт ранее
  (раздел L1/parser): `SexpPrinter.Aggregate` и
  `SexpParser.AggregatePrintRoundtrip` в `test/test_sexp.cpp` — оба проходят,
  дополнительных изменений не требуется.
- Новый файл `test/test_aggregate.cpp`: локальный `TVectorSource : ISource`
  (схема `{k: i64, v: i64}`, имена колонок хранятся в собственном члене
  `Names`, чтобы `TColumnSchema::Name` (`string_view`) не указывал на
  уничтоженный аргумент конструктора — иначе `CompileAggregate` падал с
  "unknown column 'k'", т.к. сравнение имён колонок проваливалось на
  висячих `string_view`). Хелпер `ParsePlan(sexp, source)` парсит
  `(rel aggregate (rel source "data.parquet") (keys k) (agg c count)
  (agg s sum v) (agg mn min v) (agg mx max v))` через `MakeRelParsers` с
  `SourceFactory`, затем вызывает `AnnotateTypes` + `ApplyColumnPruning` и
  возвращает корень плана. Два теста:
  - `AggregateE2E.MultipleGroups` — 12 строк / 4 группы / 2 батча,
    результат `TPhysicalPlanner::Build(...)->Next()` сравнивается с
    `std::unordered_map`-эталоном (как в L4/L5).
  - `AggregateE2E.SingleGroup` — 6 строк с одинаковым `k`, один батч, один
    выходной ряд; проверены конкретные значения `count/sum/min/max`.
  Оба теста проверяют, что второй `Next()` возвращает `false`.
- `test/CMakeLists.txt`: добавлена строка `ut(test_aggregate
  test_aggregate.cpp)`.
- `cmake -S . -B build` (повторная конфигурация для нового таргета),
  `cmake --build build -j` и полный `ctest` (4/4: test_io, test_sexp,
  test_aggregation 32/32, test_aggregate 2/2) прошли.

После L6 — CLI/parquet verification из раздела "Проверка" ниже, отдельным шагом
по решению пользователя (не входит в критерии завершения L).

**Возможные шаги после стабилизации L** (не блокируют завершение этого
раздела): составной group key `{i64,i64}` (используя generic-код из J),
несколько разных arg-колонок у разных агрегатов, агрегация без group keys
(глобальные агрегаты); замена `agg.Func ∈ {count,sum,min,max}` (предопределённый
набор на уровне `TAggregateSpec`/sexp, текущая Stage 1 поверхность) на
произвольные кастомные reduce-функции — механизм `reduce_0..reduce_{N-1}`
(L2a, `GenReducerFunDecls`) уже generic по составу/количеству, не хватает
только синтаксиса на уровне оператора/sexp для передачи тела редьюсера вместо
имени из фиксированного набора.

---

## M. Generic key pipeline: от типа logical plan до специализированного AST

Цель этапа: `rh_*` существует в одном generic-варианте с
`<named Key (template readable mutable)>`. Concrete key representation,
`rh_hash(Key)` и `rh_key_equal(Key, Key)` добавляются в AST конкретного запроса.
После объединения библиотек и generated declarations весь top-level block один
раз проходит штатные resolver, overload specialization, type annotation,
lowering и LLVM codegen. C++ не реализует probing/hash-table алгоритм.

На первом проходе обобщаются **group keys**. Aggregate state остаётся `i64`,
чтобы не смешивать generic hash table с отдельной задачей heterogeneous reducer
state. Typed aggregate states выделены в M12.

### M0. Зафиксировать рабочий i64 generic baseline

- [x] До смены ABI существующие compiler/exec/planner/sexp tests служили
  baseline для `{key:i64; count/sum/min/max:i64}`.
- [x] Добавить отдельный parity test для полностью нового generic compiler path на текущем
  запросе `{key:i64; count/sum/min/max:i64}`.
- [x] После перехода external `HashTable` на opaque key storage legacy
  `table_lifecycle.oz`/`table_grow.oz`/`count.oz`/`finalize.oz` считаются
  намеренно несовместимыми с новым ABI: их тесты не чинить. Удалить legacy
  kernels/tests после прохождения generic compiler, exec, planner и sexp E2E.
- [x] Текущие ABI `TAggregateDispatch`/`TAggregateFinalize` и
  `TRuntimeAggregate` являются только временным i64 compatibility contract,
  не целевым generic contract.

### M1. Описатель concrete key на стороне compiler

- [x] Ввести `TAggregateKeyDescriptor`, строящийся из `inputType` и
  `groupKeys`: concrete `KeyType`, список полей/column indices, byte size,
  alignment и способ materialize/project ключа.
- [x] Для одного scalar key использовать его реальный Qumir type, без
  принудительного cast к `i64`.
- [x] Для нескольких group keys генерировать детерминированное query-local имя
  named struct type из имён и типов полей; descriptor строит layout и поля
  `key_0..key_N`.
- [x] Добавить явную проверку поддерживаемости representation: сначала только
  fixed-width trivially-copyable scalar/struct fields; строки и nullable keys
  отклоняются понятной ошибкой до kernel compilation.
- [x] Unit tests descriptor: integer widths, `u64`, `f64`, `{i64,i64}`,
  `{i64,f64}` и padding `{i8,i64}`; проверяются type/layout/column mapping и
  missing/empty/string errors.

### M2. Разделить `.oz` библиотеки по ответственности

- [x] Создать `qdb/kernel/aggregation/robin_hood_generic.oz`: generic
  lookup/insert/displacement над `Key`, без `HashTable` и concrete hash bodies.
- [x] Создать отдельный `robin_hood_rehash_generic.oz`: перенос probe table с
  сохранением dense `SlotId`, без aggregate buffers и lifecycle.
- [ ] Добавить generic invariant helpers и stress/property tests для длинных
  collision chains, duplicate lookup и заполненной таблицы.
- [x] Создать production-shaped `aggregation_hashtable_generic.oz` над
  external `<ref HashTable>`: полный lifecycle, dense SlotId, `AggBuffers`,
  grow/rehash и вызов `agg_apply_reducers`; key storage используется только
  через generic `rh_*` API. i64 test выполняет duplicate/new workload и grow
  `4 -> 8 -> 16` через именованную kernel entry.
- [x] Standalone generic table lifecycle управляет opaque `u8*` key storage,
  `Dist`/`SlotId`, capacity/size, выполняет grow через generic rehash и destroy;
  C++-тест проходит несколько grow и проверяет stable dense ids. Перенос
  `GroupKeys`/`AggBuffers` остаётся следующим подшагом.
- [x] Отдельный `aggregation_dense_generic.oz` управляет dense
  `GroupKeys<Key>` и `AggBuffers[i64]`: init/update/grow/destroy; grow копирует
  `[0..Size)` без перестановки SlotId. Standalone test с generated
  `sum/count` reducers проверяет новые/повторные update и grow `4 -> 8`.
- [x] Подключить `aggregation_update_generic.oz`, объединяющий probe grow,
  dense grow, upsert и `agg_apply_reducers`, и проверить единым standalone
  workload до переноса функций на external `HashTable`: mixed duplicate/new
  i64 keys, несколько grow, generated `sum/count`, проверка dense key/state.
- [x] Создать `aggregation_finalize_states.oz`: `.oz` библиотека копирует только
  aggregate state, а generated per-query finalize wrapper обходит dense
  `GroupKeys<Key>` и раскладывает ключ обратно по output columns.
- [x] Проходящие standalone `generic_pair_*.oz` оставлены regression examples и
  не используются как production library. Несовместимые concrete
  `count.oz`/`finalize.oz`/`table_lifecycle.oz`/`table_grow.oz` и связанные
  tests удалены после полного перехода на generic production path.
- [x] `ParseFunctionLibrary` загружает новые generic файлы целиком без
  exclude-наборов по старым concrete именам.

### M3. Сделать layout `HashTable` независимым от Key

- [x] Заменить key-owned поля external `HashTable` (`Keys`, `GroupKeys`,
  `Scratch`, `Scratch2`, `QueryKey`) с `<ptr i64>` на opaque `<ptr u8>`;
  `Dist`/`SlotId` остаются `<ptr i64>`.
- [x] C++ runtime и новый generic allocator работают только с
  `uint8_t*`/числом байт и никогда не
  приводят key storage к C++-типу ключа. Concrete `<ptr Key>` существует только
  внутри объединённого typed Qumir AST конкретного запроса.
- [x] Добавить `KeySize` в metadata таблицы и передавать его в init; grow/rehash
  читают сохранённое значение. Не использовать
  скрытую константу `8`.
- [x] Первый generated/specialized typed adapter в AST делает cast
  `<ptr u8> -> <ptr <named Key (template readable mutable)>>` и передаёт typed
  pointer в generic Oz upsert; cast локализован на границе storage/algorithm.
  Rehash/finalize adapters добавляются вместе с lifecycle.
- [x] Allocation overflow считать как `capacity * KeySize`; отдельно оставить
  `capacity * sizeof(i64)` для `Dist`/`SlotId`/i64 aggregate states.
- [x] Обновить C++ mirror layout, `kHashTableSize`, static assertions и destroy
  paths одним атомарным изменением.
- [ ] ABI/layout tests: offsets и sizeof совпадают между `QumirDbModule`, C++
  runtime и тестовым mirror. C++ test mirror уже проверяет `sizeof=104` и
  ключевые offsets; остаётся проверить список/типы external fields программно.

### M4. Генерация и инъекция key operations

- [x] Добавить `GenKeyOperationFunDecls(TAggregateKeyDescriptor)`; результат —
  concrete overload declarations с фиксированными именами:
  `rh_hash(Key)->i64`, `rh_key_equal(Key,Key)->bool` и при необходимости
  `rh_key_copy(Key)->Key`. Первый реализованный backend — scalar i64; остальные
  representations остаются следующими подшагами.
- [x] Scalar integer hash работает по полной width/signedness representation:
  signed значение сначала приводится к unsigned того же width, затем zero-extend
  в `u64` и проходит общий mixer. Equality использует concrete type без cast
  ключа к `i64`. Generic i32 update проходит Selection, duplicate keys, grow и
  typed finalize с `KeySize=4`.
- [x] Вынести построение hash/equality expressions в рекурсивные helpers,
  принимающие `TTypePtr` и выражение значения. Primitive integer leaf использует
  уже реализованный width-aware backend; позднее рядом добавляются `f64` и другие
  fixed-width leaf-типы.
- [x] Struct hash строится обходом `TStructType::Fields` в объявленном порядке.
  Для каждого поля generator создаёт `TFieldAccessExpr`, рекурсивно получает
  field hash и добавляет его в стабильный `hash_combine(seed, field_hash)`.
  Имена колонок и C++ layout bytes в hash не входят; семантика определяется AST
  типа и последовательностью его полей.
- [x] Struct equality строится тем же рекурсивным обходом как `&&` сравнений
  полей слева направо, с естественным short-circuit. Generic Robin Hood
  functions при этом не меняются.
- [x] Формула и константы hash contract зафиксированы golden tests: scalar
  `xorshift64*` multiplier `2685821657736338717`, ordered combine constant
  `0x9e3779b97f4a7c15`. Tests проверяют exact hashes, порядок полей, отличие
  nested shape от flat fields и одинаковый результат двух независимо
  сгенерированных программ для одного AST-типа.
- [x] Generated `{i64,i64}` smoke компилирует `rh_hash/rh_key_equal` напрямую,
  проверяет оба поля, зависимость hash от порядка и отсутствие handwritten
  pair-specific overload в production generator.
- [x] Composite composition добавляет `TTypeDeclStmt` для generated named Key
  перед функциями update/finalize; resolver не зависит от внешней ручной
  декларации типа.
- [x] Для `f64` закреплён contract: `-0 == +0` и одинаковый hash; все NaN
  канонизируются в одну group-key equivalence class с одинаковым hash.
  Получение IEEE bits временно использует `qdb_f64_bits`, поскольку в Qumir
  нет scalar bitcast; ограничение записано в `docs/issues`.
- [x] Зафиксирована исследовательская задача Qumir на language-defined
  AST/type transforms: генерация hash/equality по описанию struct type могла бы
  выполняться на Oz/Qumir стороне. Текущий C++ traversal остаётся рабочим
  bootstrap и не блокируется этой задачей со звёздочкой.
- [x] Compile-time matrix test заново собирает одни и те же generic `.oz`
  библиотеки с generated overload declarations для `i32`, `i64`, `f64`,
  `{i64,i64}`, `{i64,f64}`. Для каждого типа успешно компилируются explicit
  entries `agg_dispatch`/`agg_finalize`, а dispatch выполняет concrete
  init/destroy с ожидаемым `KeySize`.
- [x] Первый composition smoke объединяет generic upsert/rehash с отдельными
  concrete i64 key operations и исполняет полученный AST.

### M5. Generated key materialization из TRowSet

- [x] Добавлен отдельный `GenGenericAggregateDispatchAst`, получающий
  `TAggregateKeyDescriptor`, а не `keyField:string`; scalar key column
  извлекается из `TColumn.Data` как `<ptr Key>`. Legacy generator оставлен
  только до удаления concrete tests/path.
- [x] Scalar wrapper передаёт `keys[i]` непосредственно в generic
  `agg_update<Key>`.
- [x] Composite wrapper строит typed struct literal `Key{field_0=..., ...}` на
  каждой строке и передаёт его по value; probing не знает число/типы полей.
- [x] Удалить hardcoded `<ptr i64>` finalize ABI и проверку `group key must be
  i64` из `CompileAggregate` для scalar integer keys. C++ runtime выделяет
  opaque byte buffer размера `group_count * KeySize`, generated wrapper пишет
  туда через concrete `<ptr Key>`.
- [x] i64 test выполняет два batch, один с `Selection`, duplicate/new keys и
  grow до capacity 16 через generated `agg_dispatch`.
- [x] Production scalar-integer path читает narrow integer argument column
  через concrete `<ptr T>` и приводит row value к текущему reducer contract
  `i64`. Integer `sum/min/max/count` output schema соответствует физическому
  `i64` state buffer; typed reducer states остаются отдельным этапом.
- [ ] Повторить Selection/multi-batch tests для следующих поддержанных key
  representations после расширения key operations.
- [x] Update-only `{i64,i64}` test материализует ключ из двух column buffers,
  проходит Selection, duplicate/new keys, grow до capacity 16 и сверяет dense
  keys/reducer states. Composite finalize/output SoA остаётся M7.
- [x] `build/filter_aggregate.sh` и DuckDB reference работают на исходном
  `orders.parquet` без `orders_i64.parquet`: `o_custkey:i32`,
  `o_orderkey:i32`, 35 357 групп, полный отсортированный CSV совпадает.
- [x] Добавлены отдельные qdb/DuckDB scripts для composite
  `{o_custkey:i32, o_shippriority:i32}` key; outputs сортируются по двум ключам
  и полностью совпадают при прямом CSV diff: 35 357 строк на исходном
  `orders.parquet`.
- [x] Добавлены qdb/DuckDB scripts для mixed
  `{o_custkey:i32, o_totalprice:f64}` key. На исходном `orders.parquet`
  совпадают все 36 079 отсортированных строк; DuckDB reference нормализует
  только текстовый suffix `.0` у integral `f64`.

### M6. Единый AST composition pipeline

- [x] Ввести явный `BuildGenericAggregateProgramAst(...)`, который собирает statements
  строго в порядке зависимостей:
  `pragma/type declarations -> key overloads -> reducers ->
  agg_apply_reducers -> generic RH library -> generic table library ->
  generated dispatch entry`.
- [x] Finalize program собирается аналогично:
  `type declarations -> key operations/projection helpers -> generic finalize
  library -> generated finalize entry`; scalar wrapper компилируется с explicit
  `entryName="agg_finalize"`.
- [x] Каждый parsed/generated `FunDecl` принадлежит ровно одному program AST:
  update/finalize builders независимо парсят `.oz` и генерируют declarations;
  specialization matrix строит свежую пару AST и runner-ов для каждого key type.
- [x] Update block компилируется одним `CompileKernelAst` с explicit
  `entryName="agg_dispatch"` и
  `AllowOverloads=true`; specialization выполняется Qumir pipeline после merge,
  а не вручную в C++.
- [x] В Qumir добавлен overload `CompileKernelAst(ast, entryName, error)`,
  выбирающий публичную функцию по source name после lowering. Новый generic
  HashTable test использует `aht_drive` и больше не зависит от `.back()`.
- [x] Production `CompileAggregate` переведён на explicit entries
  `agg_dispatch`/`agg_finalize` и generic update/finalize builders для i64.
- [x] Перевести оставшиеся standalone generic tests на
  explicit entry name; после этого удалить warmup/pre-specialization обходы и
  устаревшие комментарии о необходимости держать entry последней.

### M7. Typed finalize и output group-key columns

- [x] Dense `GroupKeys` хранится как AoS `Key[Capacity]`; generated finalize
  wrapper раскладывает scalar/struct fields в отдельные SoA output columns в
  порядке `groupKeys` logical operator.
- [x] Заменить `TAggregateFinalize(..., int64_t* outputKeys, ...)` на byte-oriented
  contract: C++ передаёт только `i8*` destinations/`TColumn.Data`; generated
  finalize AST приводит каждый destination к `<ptr Ti>` и пишет typed values.
- [x] `TRuntimeAggregate` выделяет каждый output key buffer как byte storage по
  `rowCount * TypeSize` с требуемым alignment, хранит его как `i8*` и строит
  `TColumn.Data`; число output columns равно `groupKeys.size() + aggs.size()`.
- [x] Проверить lifetime/alignment буферов и `DestroyAggregateRowSet`; убрать
  `std::vector<int64_t> Keys` как универсальное хранилище ключей.
- [x] Finalize/E2E tests покрывают scalar `i32/f64`, pair `{i64,i64}` и mixed
  `{i32,f64}`; decimal values проверяют сохранение representation,
  `-0/+0` и разные NaN payload проверяют canonicalization;
  порядок dense slots не считается сортировкой результата.
- [x] Из-за packed offsets Qumir IR против aligned LLVM struct ABI generated
  named Key содержит явные zeroed `u8` padding fields до descriptor offsets;
  padding исключён из hash/equality. Ограничение и желаемое исправление Qumir
  записаны в `docs/issues/qumir_struct_ir_llvm_layout_mismatch.md`.

### M8. Перешить CompileAggregate без i64 key restrictions

- [x] `CompileAggregate` строит key descriptor, key declarations/overloads,
  reducers и оба merged program AST; production больше не загружает
  `count.oz`/`finalize.oz`.
- [x] Удалить ограничения `groupKeys.size()==1` и `group key must be i64` для
  Stage 1 integer fields; float/bool пока отклоняются до генерации ядра.
- [x] Сохранить понятную раннюю диагностику unsupported variable-width/nullable
  key вместо ошибок resolver/lowering.
- [x] `TAggregateKernels` несёт output key layout metadata, необходимую exec-ноду,
  а не только `NumAggs`.
- [x] Compiler/runtime E2E покрывает `i32`, `i64`, `f64`, `{i64,i64}` и
  padding-sensitive `{i32,f64}` с grow и typed finalize output.

### M9. Pipeline/exec/planner parity gates

- [x] Новый generic path проходит i64 compiler dispatch/finalize, multi-batch
  exec grow/collision и оба planner/sexp E2E tests без изменения logical sexp
  и результатов. Legacy standalone `.oz` tests намеренно не являются gate.
- [x] Exec test для composite `{i64,i64}` проходит sexp, pruning, planner,
  multiple batches, duplicates, grow и проверяет две отдельные output columns.
  Остальные key representations добавляются по мере реализации hash backend.
- [x] Planner test строит `TAggregateOperator` над logical schema с unused
  string, затем source после pruning возвращает переставленный physical schema
  `[value:i64, key_f64:f64, key_i32:i32]`. Descriptor использует реальные
  physical types/indices и корректно исполняет mixed `{i32,f64}` aggregate.
- [x] Sexp E2E покрывает scalar `i32/f64`, `(keys k1 k2)` для `{i64,i64}` и
  mixed `{i32,f64}`.
- [x] CLI/parquet: исходный TPC-H `o_orderkey INT32`/`o_custkey INT32` работает
  без промежуточного `orders_i64.parquet`; scalar i32, composite `{i32,i32}` и
  mixed `{i32,f64}` результаты полностью сравниваются с DuckDB.

### M10. Удалить concrete i64 production path

- [x] Production не зависит от `rh_hash_i64`, `count_lookup`,
  `count_insert_existing` и hardcoded `capacity * 8` для keys; concrete symbols
  остались только в historical `.oz` fixtures и legacy tests.
- [x] Проходящие i64/pair overload examples оставлены regression fixtures и не
  загружаются `CompileAggregate`; concrete lifecycle fixtures удалены.
- [x] `kCountOzFixedFuncs` удалён из public qdb API; exclude-набор локализован в
  legacy test helper. Production composition перечисляет generic libraries.
- [x] Aggregation README и public compiler/generator comments обновлены под
  generic integer/f64/composite keys; Stage 1 оставлен только в истории плана и
  historical fixtures.

### M11. Query context, без проектирования allocator заранее

- [ ] После стабилизации generic ABI добавить opaque nullable query context в
  dispatch/finalize/lifecycle signatures и механически протянуть через все
  generated/generic calls; первые tests передают `nullptr`.
- [ ] Managed allocator/ownership определить отдельным этапом, когда lifecycle
  query execution известен; generic key migration не должна зависеть от этого.

### M12. Typed aggregate values и custom reducer AST

Это отдельный этап после generic group keys: текущий `AggBuffers` и reducer
contract всё ещё `i64`.

- [ ] Расширить `TAggregateSpec`: хранить typed reducer AST/descriptor
  `(State prev, Value value, bool is_new) -> State`; builtin count/sum/min/max
  только генерируют такой AST.
- [ ] Разрешить разным агрегатам разные `Arg` expressions/columns и ValueType.
- [ ] Заменить `int64_t** AggBuffers` на heterogeneous typed/opaque state buffers;
  allocation/rehash/finalize для каждого reducer генерируется по его StateType.
- [ ] Добавить `sum/min/max(f64)` и mixed query, например
  `keys{i32,f64} + count:i64 + sum(f64):f64`.
- [ ] Только после typed-state E2E заявлять поддержку aggregation для любых
  поддерживаемых key/value типов.

### M13. StringView/OwnedString для filter и aggregation

Подробный proposed ABI и ownership описаны в
`docs/arch/string_values.md`. Основное решение: входная строка является
borrowed `StringView`, а хранимый `OwnedString` остаётся trivially-copyable
handle; байтами владеет таблица/arena, а не каждый экземпляр struct. QDB
kernels не используют `TStringType`/managed `string` из Qumir: логический
string schema type преобразуется в собственные named POD structs QDB.

#### Порядок реализации

Следующий шаг начинается только после прохождения tests/gate предыдущего.

- [x] **S1. POD ABI:** добавить и проверить QDB named structs `StringView` и
  `OwnedString`; generated kernel AST не содержит Qumir `TStringType`.
- [x] **S2. Общий column reader:** единый generator читает fixed-width/string
  value и validity из `TColumn`, включая i32/i64 offsets и Arrow slices.
- [x] **S3. Standalone string helpers:** отдельные `.oz` tests проходят для
  byte hash/equality/compare/copy и cross-type `StringView`/`OwnedString`.
- [x] **S4. Dual key descriptor:** для scalar/composite key строятся
  `LookupKey` с views и `StoredKey` с owned handles, включая padding/validity.
- [x] **S5. Dual-key Robin Hood:** lookup принимает borrowed `LookupKey`, probe
  storage/insert/rehash используют только `StoredKey`; standalone matrix
  проходит без подключения production pipeline.
- [x] **S6. Owned bytes lifecycle:** строковые bytes копируются только после
  lookup miss, регистрируются во владении таблицы и освобождаются один раз в
  destroy; duplicate/rehash не аллоцируют bytes.
- [x] **S7. Production aggregate update:** `CompileAggregate` принимает scalar
  string и mixed composite keys, проходит Selection/multi-batch/grow tests.
- [x] **S8. String finalize:** runtime строит variable-width output `TColumn`
  с собственными `Data/Offsets/Mask`; output переживает destroy hash table.
- [x] **S9. String filter:** predicate AST специализируется в `StringView` до
  annotation; column/column и column/literal comparisons проходят E2E.
- [ ] **S10. Общая SQL null semantics:** GROUP BY, WHERE three-valued logic и
  nullable reducer arguments одинаково работают для fixed-width и strings.
- [ ] **S11. Parquet/DuckDB gate:** GROUP BY string, mixed key и filter +
  aggregate scripts полностью совпадают с DuckDB, включая null/empty/UTF-8.

#### M13.1. Зафиксировать строковый POD ABI

- [x] Добавить в `QumirDbModule` named-типы
  `StringView{Data:<ptr u8>, Size:i64}` и
  `OwnedString{Data:<ptr u8>, Size:i64}` без managed-string semantics.
- [x] Зафиксировать тестом, что generated kernel AST не содержит
  `TStringType`, а lowering не вызывает retain/release Qumir strings.
- [x] Зафиксировать, что `Size` означает число UTF-8 bytes, embedded NUL не
  является terminator, а `{nullptr,0}` допустим для пустой строки.
- [x] Добавить layout/static tests C++ <-> Qumir AST/LLVM для обоих типов.
- [x] Не менять `externals/qumir`; если named POD или literal bridge потребует
  нового API, сначала добавить reproducer/issue и отдельно согласовать правку.

#### M13.2. Общий materializer значений колонок

- [x] Выделить из filter/aggregate generators общий
  `BuildColumnValueExpr(TColumn, row, logicalType)` или эквивалентный helper.
- [x] Для fixed-width сохранить текущий typed load; для string строить
  `StringView` из `Data`, `Offsets`, `OffsetWidth` с поддержкой i32/i64 offsets.
- [x] Учесть Arrow slice/base offset: вычитать `offsets[0]`, как уже делает
  `qdb/io/text/sink.cpp::StringViewValue`.
- [x] Unit tests: empty/non-empty UTF-8, STRING/LARGE_STRING, sliced offsets и
  fixed-width materialization.
- [x] Добавить production integration tests с `Selection` и несколькими
  batches при подключении materializer к filter/aggregate generators.
- [x] Возвращать/передавать validity из общего `TColumn.Mask` materializer так
  же, как для fixed-width типов; `StringView` не кодирует null специальным
  значением.

#### M13.3. Oz StringView operations отдельно от pipeline

- [x] Создать отдельные `.oz` helpers/tests для byte equality, lexical compare,
  stable hash и byte copy над `StringView`/`OwnedString`.
- [x] Зафиксировать hash test vectors: empty, ASCII, UTF-8 multibyte, embedded
  zero, common prefixes и разные lengths.
- [x] Проверить cross-type contract:
  `hash(StringView)==hash(OwnedString)` и
  `equal(OwnedString,StringView) => hashes equal`.
- [x] Только после standalone tests подключать helpers к merged aggregation AST.

#### M13.4. Разделить lookup key и stored key

`LookupKey` является временным borrowed представлением входной строки и живёт
не дольше update batch. `StoredKey` содержит table-owned handles и живёт до
destroy таблицы. Lookup не аллоцирует; clone в `StoredKey` выполняется только
после miss. Rehash копирует только handles, не bytes. Для fixed-width полей
concrete типы обеих representations совпадают.

- [x] Расширить key descriptor до пары representations:
  `LookupKey` использует `StringView`, `StoredKey` использует `OwnedString`;
  fixed-width leaves совпадают.
- [x] Для composite keys генерировать параллельные named structs с одинаковым
  field order/padding и рекурсивной заменой string leaves.
- [x] Генерировать overloads `rh_hash(LookupKey)`, `rh_hash(StoredKey)`,
  `rh_key_equal(StoredKey,LookupKey)` и
  `rh_key_equal(StoredKey,StoredKey)`.
- [x] Параметризовать borrowed lookup и stored insertion generic Oz функциями
  двух named template types; probe storage использует только `StoredKey`.
- [x] Подключить dual-key lookup/clone/insert и stored-only rehash в общий
  aggregation update после реализации owned byte lifecycle.
- [x] Matrix tests: scalar string, `{i64,string}`, `{string,i32}`, две строки,
  duplicates/collisions/Selection/grow/rehash.

#### M13.5. Owned storage без проектирования query allocator

Ownership helpers генерируются для всех key types. Если key не содержит
owned leaves, `LookupKey == StoredKey`, `key_owned_bytes` возвращает `0`, а
`key_clone_owned` является identity и игнорирует buffer. Общий table/update
код не должен иметь отдельную ветку для fixed-width keys.

- [x] Генерировать `key_owned_bytes(LookupKey)` и clone helper, который делает
  один contiguous allocation на все string leaves нового group key.
- [x] Реализовать и проверить standalone Oz registry owned blocks с
  transactional grow/register и единым destroy.
- [x] Разделить registry API на fallible reserve и non-allocating commit, чтобы
  update мог гарантировать rollback до изменения probe/dense state.
- [x] Добавить registry fields в `HashTable` и подключить standalone lifecycle
  к `aht_init`/`aht_destroy`.
- [x] Выполнять clone только после lookup miss; duplicate rows не должны
  аллоцировать или копировать строку.
- [x] Rehash копирует handles и не трогает owned bytes; destroy освобождает
  каждый registered block ровно один раз.
- [x] Generic grow атомарно заменяет probe и dense key buffers, сохраняет
  stable dense slots и использует один путь для fixed-width/string keys.
- [x] Generic grow переносит все `AggBuffers[0..NumAggs)` по stable dense slot;
  путь `NumAggs == 0` использует тот же lifecycle без специальных таблиц.
- [x] Обеспечить transactional pre-insert failure path: ошибка reserve или
  allocation не меняет `Size`, probe arrays и dense state; insert failure
  освобождает ещё не зарегистрированный block.
- [ ] Проверить transactional failure injection тестами allocator failures и
  убедиться, что dispatch возвращает ошибку без частичного state.
- [ ] Отдельно измерить allocation overhead; chunked/query allocator оставить
  последующей заменой за тем же ABI.

#### M13.6. String finalize и output TColumn

- [x] Заменить fixed-width-only `TAggregateOutputKey{Size,Alignment}` на
  descriptor representation с явным kind `Fixed` или `String`.
- [x] Добавить generated measure pass для total byte size каждой string key
  column; fixed-width columns тем же ABI сообщают `rowCount * Size`.
- [x] В runtime выделять `Data` и `i64[rowCount+1] Offsets`, выставлять
  `OffsetWidth=8`; output buffers принадлежат result `TRowSet`.
- [x] Generated finalize раскладывает dense `StoredKey` в SoA columns, пишет
  offsets и копирует bytes до `aht_destroy`.
- [x] E2E tests проверяют scalar/composite strings, empty/UTF-8/common-prefix,
  grow, multi-batch и отсутствие ссылок output на уничтоженную table arena.
- [x] Добавить CLI/parquet script и DuckDB reference для GROUP BY string и
  mixed `{integer,string}`. Scalar `o_orderstatus` даёт 3 группы; mixed
  `{o_custkey:i32,o_orderstatus:string}` даёт 35 725 групп. Отсортированные
  CSV из QDB и DuckDB побайтно совпадают на исходном `orders.parquet`.

#### M13.7. Подключить StringView к filter

- [x] Разделить logical predicate AST и kernel-specialized predicate AST:
  string identifiers/literals переписываются в `StringView` до финального
  resolver/type annotation; нельзя переиспользовать уже аннотированный
  managed-string predicate из текущего `ParseAndAnnotate` path.
- [x] Перевести filter field binding для `TStringType` с ошибочного
  `<ptr string>` на общий per-row `StringView` materializer.
- [x] Добавить StringView helper для `=`, `!=`, `<`, `<=`, `>`, `>=` и tests
  для column-to-column predicates. Byte compare реализован C++ primitive
  `qdb_filter_string_compare` в `QumirDbModule`; AST сравнивает его результат
  с нулём.
- [x] Добавить QDB AST lowering строкового литерала непосредственно в static
  byte storage + `StringView`; не создавать промежуточный Qumir managed string.
- [x] Добавить predicates `column = "literal"` и обратный порядок operands,
  включая UTF-8, embedded NUL и empty literal.
- [x] E2E gate: parquet filter `o_orderstatus = "F"` feeding aggregation by
  string key `o_clerk` даёт 10 000 групп; отсортированный CSV полностью
  совпадает с DuckDB на исходном `orders.parquet`.

#### M13.8. Общая nullable semantics для всех типов

Физическое представление уже едино: `TColumn.Mask + MaskBitOffset`. Этот этап
не является частью строкового ABI и должен одинаково работать для integer,
f64, bool и string.

- [x] Общий column materializer возвращает value и validity и никогда не читает
  payload null-строки/null-fixed-width значения.
- [x] Generated `LookupKey`/`StoredKey` несут validity каждого nullable key
  field; null в одной key position равен null для GROUP BY, а null не равен
  любому non-null значению. Payload null field не участвует в hash/equality.
- [x] Finalize создаёт output `TColumn.Mask` для nullable group-key columns;
  null string остаётся отличим от valid empty string.
- [x] Filter реализует WHERE contract: unknown predicate не выбирает строку;
  для `AND`/`OR`/`NOT` добавить корректную three-valued logic, а не только
  финальную проверку mask.
- [x] Reducer materialization различает `count(*)` и `count(arg)`; `count(arg)`,
  `sum`, `min`, `max` пропускают null arguments. Nullable aggregate output и
  empty-input semantics фиксируются отдельными тестами.
- [x] Matrix tests покрывают null scalar/composite keys, null vs empty string,
  null aggregate arguments, Selection и несколько batches для всех
  поддерживаемых physical types.

#### M13.9. `TNullable` — qdb-локальный тип для статической nullability

M13.10 (ниже) убирает nullable machinery (per-row validity, three-valued
filter logic, `valid_N` в composite key) для колонок, которые физически не
могут содержать NULL — но для этого нужен **статический** сигнал nullability,
доступный на этапе построения AST кернела. M13.9 вводит этот сигнал —
qdb-локальный тип `TNullable` — и подтверждает, что он безопасно проходит
через существующий конвейер схем без единой правки в `externals/qumir`. Сам
по себе M13.9 не меняет генерируемый код и не влияет на perf: до M13.10
`TNullable` нигде не читается.

Подтверждено: в `~/Projects/tpch/pq100/orders.parquet` Arrow репортит
`nullable=False` для всех 9 колонок (`o_orderkey`, ..., `o_comment`) —
статический сигнал для perf gate из M13.10 на этом датасете реально доступен.

##### Архитектурное решение: `TNullable`

`NQumir::NAst::TType`/`TMaybeType` (`externals/qumir/qumir/parser/type.h`) —
открытая виртуальная иерархия с диспетчеризацией по строке `TypeName()`, а не
закрытый `variant` (так уже устроен `TNamedType`). Поэтому nullability можно
закодировать как qdb-локальный тип `TNullable : TType` (`TypeName() ==
"Nullable"`, поле `UnderlyingType`), **без единой правки в
`externals/qumir`** — при условии, что любой AST-узел, уходящий в
annotator/LLVM codegen Qumir, строится из *unwrapped* типа; `TNullable`
qumir никогда не видит. Это даёт единый источник истины: nullability едет
прямо в `TTypePtr` через существующие `TColumnSchema.Type` /
`TStructType::Fields` / `TAggregateKeyField.Type`, без отдельной
параллельной карты.

##### Инвариант: `TNullable` никогда не доходит до `NCore::PrintType`

`NCore::TPrinter::PrintType` (`externals/qumir/qumir/parser/core/printer.cpp:120-174`)
— закрытый `if/else` по конкретным подклассам `TType` (`TIntegerType` ...
`TStructType`) с fallback `throw std::runtime_error("unsupported type for
core print: " + TypeName())` на 172-й строке — точно та же схема, что и для
AST-узлов (`"unsupported AST node for core print: " + NodeName()`,
printer.cpp:740). `TNullable` (`TypeName() == "Nullable"`) в этот `if/else`
не входит и не будет входить — если `TTypePtr` с таким `TypeName()` попадёт в
`.Type` любого узла, уходящего в `PrintKernelAst`/`NCore::PrintAst` (включён,
когда `Diagnostics_ != nullptr`), компиляция упадёт с этим throw.

Контейнмент достигается без правок `externals/qumir`: `TNullable` живёт
только во *входных* типах — `TColumnSchema.Type` / `inputType.Fields[i].second`
(M13.9 S1) — и снимается (`UnwrapNullableType`) на границе, до того как тип
попадёт в `.Type` какого-либо AST-узла кернела. Это контролирует M13.10:
  - `BuildColumnValueAst` (M13.10 S1) снимает `TNullable` сама и возвращает
    unwrapped `ValueType`.
  - `BuildAggregateKeyDescriptor`/`RepresentKeyType`/`TypeLayout` (M13.10 S3)
    снимают `TNullable` на входе и пишут unwrapped тип в
    `TAggregateKeyField.Type` / `LookupType`/`StoredType`/`KeyType` — дальше
    по конвейеру (`gen.cpp:1237, 1296, 1328, 1433, 1457`, `compiler.cpp:174,
    287`) `TNullable` не встречается.

Кроме перечисленного есть ровно два места, где `inputType.Fields[i].second` /
`arg->second` читаются *напрямую*, до `BuildColumnValueAst`/дескриптора —
их нужно поправить тем же приёмом (`UnwrapNullableType` рядом с существующим
`UnwrapNamedType`), иначе либо ловится неверная классификация, либо
сработает `throw` из `PrintType`:
  - `gen.cpp:840,909` (`GenFilterKernelAst`, classify `stringFields` vs
    `fixedValues`) — без `UnwrapNullableType` перед `UnwrapNamedType(type)`
    nullable string-колонка в WHERE классифицируется как `fixedValues` и
    теряет string-специализацию предиката. Не приводит к throw в `PrintType`
    (тип не идёт в AST), но это корректностный баг. Часть M13.10 S2.
  - `gen.cpp:1062,1067,1069` (`GenGenericAggregateDispatchAst`, `argField`) —
    `TMaybeType<TIntegerType>(UnwrapNamedType(arg->second))` ложно отбрасывает
    `sum(nullable_int_column)` ("aggregate argument must be integer"), а если
    каким-то образом дошло — `valueType = arg->second` даёт
    `TPointerType(TNullable<i64>)` в `.Type` узла `values`, и печать
    `aggregate.update` падает с `"unsupported type for core print:
    Nullable"`. Добавить `UnwrapNullableType` перед обоими использованиями.
    Часть M13.10 S1 (по смыслу — тот же column-value плубинг).

Итог: правка printer/parser в `externals/qumir` для M13.9/M13.10 **не
требуется** — при перечисленных выше точках unwrap'а (все — локальные
однострочные правки по существующему шаблону `UnwrapNamedType`, без новой
инфраструктуры) `TNullable` остаётся целиком qdb-локальным и annotator/LLVM
codegen Qumir его никогда не видит. Если несмотря на это M13.10 S6 поймает
`"unsupported type for core print: Nullable"` — это сигнал о пропущенной
точке unwrap'а; чинить локально (добавить недостающий `UnwrapNullableType`),
а не как prerequisite-правку printer'а.

##### Реализация

- [x] **S1. `TNullable` + nullable из Arrow schema.** Новая директория
  `qdb/types/` для qdb-локальных `NQumir::NAst::TType`-наследников (этот тип
  в `externals/qumir` пока не тащим). `qdb/types/nullable.h`: `struct
  TNullable : NQumir::NAst::TType` (`TypeId="Nullable"`, `UnderlyingType`),
  плюс `IsNullableType(TTypePtr)` / `UnwrapNullableType(TTypePtr)` по образцу
  `UnwrapNamedType`. В
  `qdb/io/parquet/source.cpp` (цикл построения `Columns_`, где сейчас
  `.Type = ArrowTypeToQumir(field->type())`): если `field->nullable()`,
  оборачивать результат в `TNullable`. Gate: build проходит; новый unit
  test на round-trip `IsNullableType`/`UnwrapNullableType` и на то, что
  `StructTypeFromSchema` (`qdb/ops/source.cpp`) пробрасывает `TNullable`
  как обычный `TTypePtr` без изменений — поведение остальных тестов не
  меняется (никто пока `TNullable` не читает).

---

#### M13.10. Не генерировать nullable machinery для статически non-nullable типов

M13.8 добавила per-row validity (`_valid`/`_value` через `BuildColumnValueAst`),
three-valued filter logic (`BuildFilterTruthAst`/`FilterExprValidity` +
`qdb_sql_bool_and/or/not`) и `valid_N`-поля composite key безусловно, для всех
типов — даже когда колонка физически не может содержать NULL. Замер на
`filter_aggregate_string.sh` (`o_totalprice > 400000.0`, GROUP BY
`o_orderstatus`, 150M строк `orders.parquet`, ни одна из колонок не nullable)
показал ~0.5s регрессии: per-row невстраиваемый `tail call @qdb_sql_bool_and`
в filter loop (declare-only функция, `ab85ec4`) плюс лишний branch/zero-init
на каждое чтение колонки (`b3a88ab`) плюс лишнее поле `valid_0` (+ padding) в
hash/equal композитного ключа (`78d17f9`/`62feecf`).

Зависит от M13.9 (`TNullable`/`IsNullableType`/`UnwrapNullableType`,
`qdb/types/nullable.h`) — начинается только после прохождения его gate.

##### Порядок реализации

Следующий шаг начинается только после прохождения build + tests/gate
предыдущего.

- [x] **S1. `BuildColumnValueAst`: fast path для non-nullable.**
  `qdb/kernel/column_value.cpp`: в начале — `nullable =
  IsNullableType(logicalType)`, `type = UnwrapNamedType(nullable ?
  UnwrapNullableType(logicalType) : logicalType)`. Для `nullable == false` —
  во всех трёх ветках (int/float, bool, string) вернуть безусловный
  `data[i]`/StringView load без `_valid` var, без `if(valid){...}`, без
  zero-init; `IsValid` = `TNumberExpr(true)` с `TBoolType`. Для `nullable ==
  true` — текущая ветка M13.8 без изменений (только работает с unwrapped
  типом). `ValueType` в результате никогда не содержит `TNullable`.
  Вызывающий код (`GenFilterKernelAst`, `GenGenericAggregateDispatchAst`) не
  меняется — он просто передаёт тип, который может быть `Nullable<...>`.
  Gate: unit-тесты `BuildColumnValueAst` для int/float/bool/string ×
  {nullable, non-nullable} — у non-nullable в AST нет `_valid`/`if`/`bitoff`,
  у nullable AST не изменился.
  Дополнительно (тот же приём, другая функция): в
  `GenGenericAggregateDispatchAst` (`gen.cpp:1062,1067,1069`, `argField`) —
  добавить `UnwrapNullableType` перед `UnwrapNamedType(arg->second)` (классиф.
  `argType`) и перед `valueType = arg->second`/`TPointerType(valueType)`; см.
  инвариант `TNullable`/`PrintType` в M13.9 — без этого `sum(nullable_int_column)`
  либо ложно отбрасывается, либо роняет печать `aggregate.update`. Gate:
  unit-тест на `sum`/`min`/`max` по nullable integer column — компилируется,
  AST узла `values` имеет unwrapped тип.
- [x] **S2. Filter: пропуск three-valued machinery без nullable полей.**
  `gen.cpp::GenFilterKernelAst`: при построении `validityNames` (~строка 912)
  добавлять запись только если `IsNullableType(type)` для исходного типа
  поля. После цикла — если `validityNames.empty()`, собрать `selected =
  cast(predicate, u8Type)` напрямую (путь до `ab85ec4`), полностью пропуская
  `BuildFilterTruthAst`/`filter_truth_state`/`nextTruthTemporary`; иначе —
  текущий путь без изменений. Gate: `test_filter.cpp` — схема без nullable
  колонок даёт IR без `qdb_sql_bool_*`/`filter_truth_state`; nullable/mixed
  схемы — без регрессии относительно текущих M13.8 тестов.
  Дополнительно: classify `stringFields`/`fixedValues` (`gen.cpp:840,909`) —
  добавить `UnwrapNullableType` перед `UnwrapNamedType(type)`, иначе nullable
  string-колонка теряет string-специализацию предиката (см. инвариант
  `TNullable`/`PrintType` в M13.9). Gate: WHERE по nullable string-колонке —
  предикат специализирован так же, как для non-nullable string-колонки.
- [x] **S3. `BuildAggregateKeyDescriptor`/`RepresentKeyType`: условный
  `valid_N`, `TAggregateKeyField.Type` всегда unwrapped.**
  `qdb/kernel/aggregate_key.h`: добавить `bool IsNullable = false;` в
  `TAggregateKeyField`. `RepresentKeyType` (`aggregate_key.cpp`): сначала
  `nullable = IsNullableType(original)`, `inner = nullable ?
  UnwrapNullableType(original) : original`, дальше диспетчеризация по
  `UnwrapNamedType(inner)` как раньше; везде, где сейчас используется
  `original` (`.Lookup`/`.Stored` в int/float/bool-ветке и аргумент
  `TypeLayout(original)`), использовать `inner` — иначе `TNullable`
  просочится в `LookupType`/`StoredType`/`Layout`. `TRepresentedType`
  получает поля `Nullable: bool` и `Inner: TTypePtr` (= `inner`). В цикле
  `BuildAggregateKeyDescriptor` (`aggregate_key.cpp:184-224`) добавлять
  `valid_N`-поле и связанный +1-байтовый offset/padding только если
  `represented.Nullable`; `result.Fields.back().Type = represented.Inner`
  (НЕ исходный `inputType.Fields[i].second` — он может быть `TNullable<...>`,
  а `TAggregateKeyField.Type` должен оставаться unwrapped, см. инвариант
  `TNullable`/`PrintType` в M13.9); `result.Fields.back().IsNullable =
  represented.Nullable`. Gate: новые golden layout тесты в
  `test_aggregation.cpp` — non-nullable scalar/composite key совпадает по
  `Size`/`Alignment`/набору полей с состоянием **до `78d17f9`** (без
  `valid_N`); nullable — без изменений; mixed (1 nullable + 1 non-nullable
  поле) — `valid_N` только для nullable поля, offsets корректны;
  дополнительно — для всех полей `!IsNullableType(field.Type)`.
- [x] **S4. `HashKeyValue`/`EqualKeyValue`/`KeyOwnedBytesExpr`: смотреть на
  наличие `valid_N`, а не предполагать его.** В `gen.cpp` добавить
  небольшой helper (например `HasValidityField(structType, fieldIndex)` —
  проверка наличия соседнего поля `valid_<N>`). В блоках `if
  (fieldName.starts_with("key_"))` всех трёх функций (~строки 280, 365, 493)
  оборачивать в `TIfExpr(valid_N, ...)` / `(!left.valid_N || ...)` только
  если helper вернул true; иначе — голый `fieldHash`/`fieldEqual`/
  `fieldBytes`, как до `78d17f9`. `CloneKeyValue`/`ZeroValueExpr` — проверить
  после S3, изменения, скорее всего, не нужны (просто не встретят `valid_N`
  в списке полей). Gate: golden hash/equal/bytes константы и сгенерированные
  `__overload_*` — для non-nullable composite key побайтно совпадают с
  baseline **до `78d17f9`**; mixed-кейс получает новый закреплённый golden.
- [x] **S5. Dispatch/Finalize/exec: условная output `Mask`.**
  `GenGenericAggregateDispatchAst` (`gen.cpp:1111-1132`) — цикл уже
  обрабатывает `valid_N` только если оно есть в `keyStruct.Fields`; после S3
  должен заработать без изменений (проверить build+test).
  `GenGenericAggregateFinalizeAst` (`gen.cpp:1320-1326`) — сейчас безусловно
  эмитит `qdb_bitmap_set_valid(output_mask_N, slot, keyValue.valid_N)`;
  завернуть в `if (HasValidityField(...))` (helper из S4), для non-nullable
  поля не трогать `output_mask_N` вовсе. `aggregate_exec.cpp` (~107-122) —
  для non-nullable group-key столбца выходной `TColumn.Mask = nullptr`
  (не подключать `keyBuffer.Mask.data()`). Gate: E2E finalize тест —
  non-nullable group-by output column имеет `Mask == nullptr`; nullable —
  без изменений.
- [x] **S6. Matrix tests + perf regression gate.** Матрица {non-nullable,
  nullable, mixed} × {scalar key, composite key} × {filter по
  nullable/non-nullable колонке} в `test_filter.cpp`/`test_aggregation.cpp`:
  корректность результатов (не хуже текущих M13.8 nullable-тестов) и форма
  AST (нет лишних `valid_N`/`qdb_sql_bool_*`/`_valid` там, где их не должно
  быть). Повторный прогон `filter_aggregate_string.sh`/`filter_aggregate.sh`
  на `orders.parquet` — время возвращается к уровню до M13.8 (±шум, т.е.
  обратно к ~0.5s быстрее текущих ~4.46s).

---

## Проверка (verification)

- Сборка: `cd build && ninja` — без ошибок.
- `cd build && ctest --output-on-failure` — проходят `test_io`, `test_sexp`,
  `test_aggregation` (32/32), `test_aggregate` (2/2).
- На stage 1 проверяем только целочисленные ключи/значения.

### E2E через CLI на parquet — выполнено

Реальные TPC-H parquet (`orders.parquet`) хранят `o_orderkey`/`o_custkey` как
`INT32` и `o_totalprice` как `DOUBLE`, а Stage 1 `CompileAggregate` требует
`i64` и для group key, и для shared value column — поэтому напрямую
`(agg ... o_orderkey)`/`(keys o_orderkey)` не работают.

Сгенерирован derived `build/orders_i64.parquet` (`o_custkey`/`o_orderkey`
приведены к `BIGINT` через `CAST`, `o_totalprice` без изменений; команда
генерации задокументирована в `filter_aggregate_duckdb.sh`).

- `build/filter_aggregate.sh` — `qdb -i <query.sexp> --format csv` на
  `(rel aggregate (rel filter (rel source "orders_i64.parquet")
  (: (> o_totalprice (: 400000.0 f64)) u8)) (keys o_custkey) (agg cnt count)
  (agg sum_orderkey sum o_orderkey) (agg min_orderkey min o_orderkey)
  (agg max_orderkey max o_orderkey))`; вывод без заголовка, отсортирован по
  `o_custkey`.
- `build/filter_aggregate_duckdb.sh` — эталон, тот же запрос через DuckDB
  (`GROUP BY o_custkey ORDER BY o_custkey`).
- Результат: **35357 групп**, `diff` между выводами qdb и DuckDB — пустой
  (полное совпадение, включая `count`/`sum`/`min`/`max`). qdb обработал весь
  `orders_i64.parquet` (full scan + aggregate) за ~0.34s (без учёта побочного
  LLVM IR dump в stdout/stderr — известная, безвредная особенность текущей
  сборки).
