# Implementation Plan

## 1. Logical plan — `qdb/ops/`

Чистое дерево `TExpr`. Никакого исполнения.

- [x] `operator.h` — `IOperator : TExpr` с `RelName()`, `Type`, `Children()`, `TMaybeOp<T>`
- [x] `source.h / source.cpp` — `TSourceOperator`
- [x] `filter.h / filter.cpp` — `TFilterOperator`, `MakeFilter(input, predicate_string)`
- [x] `project.h / project.cpp` — `TProjectOperator`, `MakeProject(input, specs)`
- [x] `parse_kernel.h / parse_kernel.cpp` — `ParseAndAnnotate`
- [ ] **Типизация операторов** (см. п.3): операторы пока выставляют `Type` сами в конструкторе; после п.3 это переедет в `pipeline/typing`

## 2. S-expression — `qdb/sexp/`

- [x] `printer.h / printer.cpp` — `MakeRelPrinters()`, формат `(rel source "path")`, `(rel filter ...)`, `(rel project ...)`
- [x] `parser.h / parser.cpp` — `MakeRelParsers(TRelParserOptions)`, `SourceFactory`
- [ ] **Упрощение project**: голый идентификатор вместо пары `(col col)` — identity-проекция

## 3. Pipeline — `qdb/pipeline/`

Два отдельных прохода над логическим деревом.

### 3a. Typing pass — `pipeline/typing.h / typing.cpp`

**Цель:** присвоить каждому оператору тип `TFunctionType([required], output)` из qumir.

- `ReturnType` = `outputColumns` (что оператор выдаёт вверх)
- `ParamTypes[0]` = `requiredColumns` (что нужно от input; изначально = полная output-схема input)
- У `TSourceOperator`: `ParamTypes = []` (нет upstream-оператора)

Тип задаётся **bottom-up** (конструктор или отдельный pass):

| Оператор | `ParamTypes[0]` (изначально) | `ReturnType` |
|---|---|---|
| Source | — | `StructTypeFromSchema(source)` |
| Filter | `input.ReturnType` (всё) | `input.ReturnType` |
| Project | `input.ReturnType` (всё) | struct из projection specs |

Добавить в `IOperator`:
```cpp
// Колонки, используемые в собственных выражениях (предикат / проекции).
// Не хранится — вычисляется на лету.
virtual std::unordered_set<std::string> ComputeReferencedColumns() const = 0;

// Хелпер: output-схема этого оператора.
NQumir::NAst::TTypePtr OutputColumns() const;
```

Реализации `ComputeReferencedColumns()`:
- Source → `{}`
- Filter → `FindUnboundVars(Predicate_)`
- Project → `⋃ FindUnboundVars(spec.Expression)`

`FindUnboundVars` уже есть в `pipeline/unbound_vars.h`.

### 3b. Column push-down — `pipeline/column_pushdown.h / column_pushdown.cpp`

**Цель:** сузить `ParamTypes[0]` каждого оператора до минимально необходимых колонок, и вызвать `SetRequiredColumns` на `TSourceOperator`.

Проход **top-down**:

```
walk(node, required_from_parent):

  if node == Project:
    required = ComputeReferencedColumns()     // project сам определяет свой вход
  else:
    required = own_refs ∪ required_from_parent  // filter/source пробрасывают

  narrowed = subset(ParamTypes[0], required)
  node.Type.ParamTypes[0] = narrowed

  if node == Source:
    node.SetRequiredColumns(required)
    return

  walk(node.Input(), required)

// Стартовый вызов:
initial = все поля ReturnType root
walk(root, initial)
```

Пример для `project(filter(source))`:

| Шаг | Оператор | `required_from_parent` | `own_refs` | Итоговый `required` |
|---|---|---|---|---|
| 1 | Project | `{a, b}` (output root) | `{x, y}` (из expr) | `{x, y}` |
| 2 | Filter | `{x, y}` | `{y, z}` (предикат) | `{x, y, z}` |
| 3 | Source | `{x, y, z}` | `{}` | `{x, y, z}` → `SetRequiredColumns` |

Заменяет текущий `ApplyColumnPruning` (bottom-up, живёт в `ops/source.cpp`).

## 4. Kernel generation — `qdb/kernel/`

- [x] Filter kernel: `GenFilterKernelSource(inputType, predicateExpr)`
- [ ] Project kernel: `GenProjectKernelSource(inputType, outputType, projectionExprs)`
- [ ] Fusion: `GenFusedFilterProjectKernelSource(...)`

## 5. Physical plan — `qdb/exec/`

- [x] `executor.h` — `IRuntimeNode`
- [x] `source_exec`, `filter_exec`, `project_exec`
- [ ] Computed project runtime
- [ ] Fused filter+project executor
- [x] `planner.cpp` — после п.3: читать `output` через `TFunctionType::ReturnType`

## 6. CLI — `bin/cli.cpp`

- [x] `-i file.sexp` — читает план из s-expression файла
- [x] `--format <attrs>name`, `--rowsets n`
- [x] `SourceFactory` создаёт `TParquetSource` по пути из `(rel source "path")`

## 7. Tests

- [x] `test/test_io.cpp`
- [x] `test/test_sexp.cpp` — printer/parser roundtrip
- [ ] `test/test_pipeline.cpp` — typing pass: проверить `Type` после типизации; push-down: проверить `ParamTypes[0]` и `RequiredColumns` на source
