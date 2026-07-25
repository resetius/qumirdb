# Adaptive Join Streaming After One Side EOF — пошаговый план

## Контекст / проблема

Текущий `TRuntimeJoin` остаётся симметричным до конца: каждая пришедшая строка
сохраняется в `TRowStore`, вставляется в свою `HashTable`, пробивает таблицу
противоположной стороны, а пары потом материализуются через
`TJoinOutputBuilder`.

Это корректно для конвейерного случая, когда обе стороны продолжают приходить
вперемешку: будущие строки противоположной стороны должны найти уже пришедшие
строки текущей стороны.

Но после EOF одной стороны симметрия больше не нужна. Если, например, `Left_`
уже закончился, то оставшийся `Right_` можно обрабатывать как поток:

- пробить строку/батч правой стороны по уже построенному `LeftTable_`;
- сразу отдать найденные пары;
- не сохранять правые батчи в `RightRows_`;
- не вставлять правые row-id в `RightTable_`.

Именно это важно для TPC-H: текущий `PullOneInputBatch()` фактически сначала
полностью вычитывает left, затем right. На больших фактовых таблицах правая
сторона после EOF left сейчас всё ещё лишне буферизуется и хэшируется.

Цель этого плана — реализовать вариант **B**: адаптивный join, который
round-robin читает обе стороны до первого EOF, а после EOF одной стороны
переключает вторую сторону в streaming/probe-only режим. Это не требует CBO и
не завязано на то, какую сторону автор запроса или планировщик поставил слева.

---

## Ключевая идея

До первого EOF:

- `ProcessLeft`: probe `RightTable_`, emit пары, insert left в `LeftTable_`,
  store left batch в `LeftRows_`.
- `ProcessRight`: probe `LeftTable_`, emit пары, insert right в `RightTable_`,
  store right batch в `RightRows_`.

После EOF одной стороны:

- завершившаяся сторона остаётся build/materialized side;
- ещё живая сторона становится stream side;
- stream side только probe'ит build table и emit'ит пары;
- stream side не попадает в свой `TRowStore` и не вставляется в свою `HashTable`;
- residual-фильтр `jt_residual_filter` продолжает выполняться внутри kernel до
  `pb_push`: одна строка берётся из store завершившейся стороны, вторая строка
  берётся из текущего batch stream side.

Корректность держится на том, что после EOF build side уже не появятся будущие
строки, которые могли бы пробивать stream side. Значит, хранить stream side
для будущих проб больше не нужно.

---

## Ограничения по join type

### INNER

Полностью подходит для adaptive streaming. Это основной целевой путь для Q9 и
обычных fact-table join'ов.

### LeftSemi / LeftAnti без residual

Текущий оптимизированный путь уже почти правильный:

- left полностью сохраняется и вставляется в `LeftTable_`;
- right вставляется через `InsertKeyOnly` без `RightRows_`;
- результат получается через `FinalizeAntiSemi`.

Этот путь не нужно ломать на первом этапе. Он уже избегает материализации
правой стороны.

### LeftSemi / LeftAnti с residual

Нужен отдельный streaming-путь, потому что residual требует доступ к колонкам
обеих сторон. Целевой вариант:

- сторона, которая стала build side, хранится в `TRowStore`;
- stream side обрабатывается текущим batch'ем без помещения в `TRowStore`;
- kernel emit'ит пары, уже отфильтрованные `jt_residual_filter`;
- runtime собирает `MatchedLeftIds_`;
- в конце emit'ит left строки по членству, как сейчас.

Если первым EOF стал `Right_`, то оставшийся left-stream нельзя просто
выкинуть: для LeftSemi/LeftAnti результат определяется left-строками. Поэтому
на первом этапе лучше сохранить fast path для left-first исполнения, а
adaptive residual semi/anti включать только когда EOF случился на left и
дальше stream'ится right. Полная симметрия для semi/anti — отдельный шаг.

### Left outer

Probe-only stream right после EOF left корректен: left хранится, matches
помечаются в `LeftTable_`, финальный `FinalizeOuter(LeftTable_, RightTable_)`
можно заменить или адаптировать так, чтобы он сканировал unmatched left.

На первом этапе можно оставить outer joins на старом симметричном пути, чтобы
не смешивать оптимизацию INNER с nullable-padding логикой.

### Right / Full outer

Не включать в первый этап. Для правой/full outer стороны нужны гарантии
трекинга unmatched rows на stream side, а stream side как раз не хранится.
Это требует отдельной структуры или частичного materialize only-unmatched.

---

## Изменения в kernel ABI

### Шаг 1 — добавить probe-only entrypoints

Файл: `qdb/kernel/compiler.h`

- [ ] Добавить в `TJoinKernels` два новых вызова рядом с `ProcessLeft/ProcessRight`:

```cpp
std::function<bool(void* build, TRowSet* streamBatch, int64_t streamBatchIdx,
    void* pairs, TRowSet* leftStore, TRowSet* rightStore)> ProbeLeftStream;
std::function<bool(void* build, TRowSet* streamBatch, int64_t streamBatchIdx,
    void* pairs, TRowSet* leftStore, TRowSet* rightStore)> ProbeRightStream;
```

Названия со стороны output semantics:

- `ProbeLeftStream`: текущий batch — left stream, build table — right.
- `ProbeRightStream`: текущий batch — right stream, build table — left.

- [ ] Реализовать контракт: функции probe'ят build table и `pb_push` пары, но
      не вызывают insert для stream side.

### Шаг 2 — сгенерировать probe-only AST

Файлы: `qdb/kernel/join_gen.*`, `qdb/kernel/join/join_update.oz`

- [ ] Добавить вариант текущего `jt_process_left/right`, который:

- читает key из `streamBatch`;
- lookup'ает `build` table;
- для каждого match вызывает тот же emit-код, что и `ProcessLeft/Right`;
- не вызывает `jt_insert` / `jb_append` для stream side.

- [ ] Не дублировать residual-логику: выделить в `.oz` общий helper:

- `jt_probe_and_emit(...)` — probe + residual + `pb_push`;
- `jt_emit_and_insert(...)` становится `jt_probe_and_emit(...)` + insert.

Тогда обычный symmetric path и новый streaming path используют один и тот же
код фильтрации и формат pair buffer.

### Шаг 3 — поддержать transient row id для stream side

Сейчас `TRowId` кодирует `(batchIdx << 32) | rowIdx` и residual читает обе
стороны через `left_store/right_store`. Для stream side batch не лежит в
`TRowStore`, значит `jt_residual_filter` не сможет найти его по `store[batch]`.

- [ ] Поддержать ABI для streaming:

- передавать в probe-only kernel указатель на текущий stream batch;
- передавать sentinel batch index для stream side, например `-1`;
- обновить generated `jt_residual_filter`, чтобы при `BatchIndex(rowId) == -1`
  читать колонку из `stream_left_batch` / `stream_right_batch`, а не из store.

- [ ] Добавить расширенную internal-сигнатуру residual:

```text
jt_residual_filter_ex(left_store, right_store,
                      stream_left_batch, stream_right_batch,
                      left_row_id, right_row_id) -> bool
```

- [ ] Обновить default always-true версию, чтобы она игнорировала новые
      аргументы.
- [ ] Старые `ProcessLeft` и `ProcessRight` должны передавать `nullptr` для
      stream batch pointers.
- [ ] Probe-only entrypoints должны передавать текущий batch на
      соответствующей стороне.

---

## Изменения в `TRuntimeJoin`

### Шаг 4 — заменить left-first pull на adaptive pull

Файлы: `qdb/exec/join_exec.h`, `qdb/exec/join_exec.cpp`

- [ ] Добавить состояние:

```cpp
enum class EJoinStreamMode {
    Symmetric,
    StreamLeftAgainstRight,
    StreamRightAgainstLeft,
};

EJoinStreamMode StreamMode_ = EJoinStreamMode::Symmetric;
EJoinSide NextPullSide_ = EJoinSide::Left;
```

- [ ] Переписать текущий `PullOneInputBatch()`:

1. В `Symmetric` режиме читать стороны round-robin.
2. Если выбранная сторона вернула batch:
   - store + `ProcessLeft/Right`;
   - `DrainKernelPairs()` или `CollectMatchedLeftIds()`.
3. Если выбранная сторона дала EOF:
   - пометить `LeftDone_` / `RightDone_`;
   - если другая сторона ещё не done, переключить `StreamMode_`;
   - больше не пытаться читать завершившуюся сторону.
4. В streaming режиме читать только живую сторону:
   - не `PushBatch` для stream side;
   - вызвать `ProbeLeftStream` или `ProbeRightStream`;
   - drain pairs;
   - `Release(&batch)` после probe, потому что runtime не сохраняет batch.

### Шаг 5 — обеспечить lifetime output rows

Проблема: `TJoinOutputBuilder` сейчас материализует output по `TRowId` из
`TRowStore`. Если stream side batch сразу `Release`, то пары из `PairBuffer_`
нельзя отложить в builder как `(stored row id, transient row id)`.

Нужен один из двух вариантов:

#### Вариант 5A — immediate materialization для streaming pairs

- [ ] Добавить отдельный builder/path, который собирает output rows сразу из:

- row id build side в `TRowStore`;
- row index stream side в текущем `TRowSet`.

Он должен наполнять owned `TRowSet` точно так же, как `TJoinOutputBuilder`, но
без сохранения stream batch после выхода из функции.

Это предпочтительный вариант: он реализует саму цель оптимизации — stream side
не хранится.

#### Вариант 5B — временный single-batch store

- [ ] Не выбирать 5B как основной путь без отдельного решения по lifetime:
      временный single-batch store проще, но при output chunking легко начать
      держать stream batch между вызовами `Next()`.

Поэтому целевой план — **5A**.

### Шаг 6 — расширить materialization helpers

Файлы: `qdb/exec/join_exec.h`, `qdb/exec/join_exec.cpp`

- [ ] Добавить helper, симметричный `TakeColumn`, но работающий с одной строкой
      из текущего `TRowSet`:

- `TakeColumnFromStore(...)` оставить как есть через `TakeColumn`;
- добавить `TakeColumnFromBatch(const TRowSet& batch, std::vector<int32_t> rows, ...)`;
- общий код fixed/string/null copy вынести так, чтобы не дублировать
  materialization semantics.

- [ ] Добавить builder/API для streaming:

```cpp
class TStreamingJoinOutputBuilder {
public:
    void AddPairsFromProbeBuffer(...);
    bool NextBatch(TRowSet& out);
    void Clear();
};
```

- [ ] Сделать API батч-ориентированным, чтобы builder не переживал release
      stream batch:

```cpp
void MaterializeStreamingPairs(TRowSet& streamBatch,
    EJoinSide streamSide,
    TPairBufferState& pairs,
    std::deque<TRowSet>& readyOutput);
```

- [ ] Materialization path сразу создаёт owned output batches и кладёт их в
      очередь `ReadyOutput_`.

### Шаг 7 — очередь готовых output batches

- [ ] В `TRuntimeJoin` добавить:

```cpp
std::deque<TRowSet> ReadyOutput_;
```

- [ ] `Next()` сначала отдаёт готовые `ReadyOutput_`, затем старый `Builder_`,
      затем pull/probe.

- [ ] Деструктор `TRuntimeJoin` должен `Release`/`Destroy` все оставшиеся ready
      outputs, если consumer не дочитал node до конца.

---

## Semi/Anti с residual

### Шаг 8 — первый безопасный вариант

- [ ] Сохранить текущий blocking path для `LeftSemi/LeftAnti + HasResidual_`
      как fallback.

После стабилизации INNER включить частичную оптимизацию:

- [ ] Если `LeftDone_` наступил первым, stream'ить right против `LeftTable_`.
- [ ] Собирать `MatchedLeftIds_` из filtered pairs.
- [ ] Не store'ить right.
- [ ] В конце emit left rows по membership.

- [ ] Если `RightDone_` наступил первым, оставить fallback на старый path.

### Шаг 9 — полный symmetric semi/anti позже

Для полной адаптивности semi/anti нужны дополнительные структуры:

- [ ] Спроектировать emit/membership для left rows, пришедших после EOF right.
- [ ] Добавить residual probe left-stream против stored right.
- [ ] Сохранять unmatched/matched статус без повторной materialization правой
      стороны.

Это отдельная задача после INNER, чтобы не размывать главный выигрыш.

---

## Outer joins

### Шаг 10 — оставить outer на старом пути

- [ ] На первом PR/итерации отключить adaptive streaming для:

- `EJoinType::Left`;
- `EJoinType::Right`;
- будущего `Full`, если появится.

Причина: outer joins требуют корректного unmatched tracking и NULL-padding. Их
лучше оптимизировать после того, как INNER streaming будет покрыт тестами и
TPC-H.

### Шаг 11 — отдельный будущий план для left outer

Left outer можно адаптировать позже:

- [ ] Stream right после EOF left.
- [ ] Probe right против stored left.
- [ ] Использовать то, что left matches уже помечаются в left table/buckets.
- [ ] Финальный unmatched-left scan не должен зависеть от `RightTable_` row-id.

Right/full outer требуют materialize или tracking stream-side unmatched rows.

---

## Тесты

### Unit tests

Файл: `test/test_join_exec.cpp`

- [ ] Добавить тесты:

- INNER: left EOF first, right stream batch не попадает в `RightRows_`, результат
  совпадает с nested-loop.
- INNER: right EOF first, left stream path.
- INNER multi-match: один stream batch даёт больше `kJoinOutputBatchRows`, output
  отдаётся несколькими `Next()` без удержания released stream batch.
- INNER + residual: residual читает stored side + transient stream batch.
- Empty side: EOF до первого batch у одной стороны.
- Existing left-first behavior: текущие тесты должны остаться зелёными.

Для проверки “не попадает в store” можно добавить test-only runtime hook или
косвенный тест через synthetic node, у которого batch memory invalidated after
`Release`; корректная immediate materialization не должна читать released memory.

### Kernel tests

Файл: `test/test_join_kernel.cpp`

- [ ] Добавить проверки probe-only entrypoints:

- `ProbeRightStream` probe'ит `LeftTable_` и не меняет `RightTable_`.
- `ProbeLeftStream` probe'ит `RightTable_` и не меняет `LeftTable_`.
- residual вызывается с transient stream batch.

### Regression / performance

- [ ] Прогнать базовую проверку:

```bash
cmake --build build -j
cd build && ctest --output-on-failure
cd build && ../benchmark/tpch/run_tpch.sh 1
```

- [ ] Отдельно проверить целевые TPC-H запросы:

```bash
cd build && ../benchmark/tpch/run_tpch.sh 1 9
cd build && ../benchmark/tpch/run_tpch.sh 1 21
```

Ожидаемый эффект:

- Q9: меньше памяти и времени на стороне `lineitem`, если она stream'ится после
  EOF меньшей стороны.
- Q21 residual semi/anti: первый выигрыш появится после шага 8; до него
  результат должен остаться корректным, но без ускорения этого path.

---

## Порядок реализации

- [ ] Добавить kernel probe-only entrypoints без включения runtime path.
- [ ] Покрыть probe-only entrypoints unit-тестами.
- [ ] Ввести `ReadyOutput_` и immediate materialization для stream side.
- [ ] Переписать INNER `PullOneInputBatch()` на round-robin + EOF streaming.
- [ ] Прогнать existing join tests и TPC-H.
- [ ] Включить INNER + residual transient-batch path.
- [ ] Прогнать Q9/Q21/full TPC-H.
- [ ] Отдельно включить частичный `LeftSemi/LeftAnti + residual`, когда EOF
      first у left и stream side — right.
- [ ] Оставить outer joins на старом пути до отдельного плана.

---

## Инварианты

| Инвариант | Требование |
|---|---|
| Нет double emit | Stream side после EOF не вставляется в hash table |
| Нет dangling reads | Output из stream side материализуется до `Release(&batch)` |
| Residual до emit | `jt_residual_filter` остаётся перед `pb_push` |
| Existing pair format | Stored/stored пары используют текущий `TRowId`; stream пары не переживают текущий batch |
| CBO не требуется | Выбор build side определяется фактическим первым EOF |
| Outer не ломается | Outer joins временно остаются на старом symmetric path |

---

## Риски

- Самый рискованный кусок — materialization из transient stream batch. Его надо
  покрыть тестом на multi-output-batch, иначе легко случайно удержать released
  batch.
- Расширение residual ABI должно сохранить нулевую цену для default
  always-true фильтра после `-O3`.
- Round-robin pull может изменить порядок output rows. Тесты должны сравнивать
  join результат как multiset там, где порядок не является контрактом.
- `TRowStore::Data()` может инвалидироваться после `PushBatch`; kernel calls
  должны продолжать перечитывать `Data()` на каждый вызов, как сейчас.
