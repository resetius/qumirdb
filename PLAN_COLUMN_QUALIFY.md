# Plan: Column Qualification Pass

## Проблема

Q7 падает с «join output column name 'n_nationkey' appears on both sides» —
два `rel source` на одну таблицу `NATION` оба выдают колонку `n_nationkey`.
После первого join-а `n_nationkey` уже в выходной схеме; второй join с NATION
добавляет её снова → конфликт в `ComputeJoinOutputType`.

Корневая причина: колонки живут без квалификатора источника. Исправление —
ранний проход, который присваивает каждому источнику уникальный алиас и
переименовывает все его колонки в `alias.col_name`. После этого двух
`n_nationkey` не бывает по построению.

## Дизайн

### Алиас источника

Алиас = стем имени файла без расширения, приведённый к нижнему регистру
(`/path/to/nation.parquet` → `nation`). Если несколько источников дают один
и тот же стем, они различаются числовым суффиксом:
`nation_0`, `nation_1`, …

Дополнительно sexp позволяет задать явный алиас:
```
(rel source "__NATION__" "n1")   ; явный алиас
(rel source "__NATION__")        ; алиас по стему: nation / nation_0 / ...
```

`TSourceOperator` получает поле `Alias_: std::string`.

### Формат квалифицированного имени

`alias.column_name` — разделитель `.` удобен для чтения и не конфликтует
с существующими именами (в parquet-схемах точки в именах колонок не
используются).

Примеры после нормализации:
- `n_nationkey` из первого NATION → `nation_0.n_nationkey`
- `n_nationkey` из второго NATION → `nation_1.n_nationkey`
- `n_name` из первого NATION    → `nation_0.n_name`

## Проходы (порядок в pipeline)

```
ParseSexp
  → AssignSourceAliases   [new] назначить алиасы всем источникам
  → QualifyColumns        [new] переименовать схемы + обновить refs
  → AnnotateTypes         [existing]
  → ApplyColumnPruning    [existing]
  → TPhysicalPlanner::Build
```

### Pass 1: `AssignSourceAliases`

Обходит дерево операторов в любом порядке, собирает все `TSourceOperator`,
группирует по стему пути, назначает алиасы:
- если стем уникален → алиас = стем (без суффикса)
- если дублируется  → `stem_0`, `stem_1`, … в порядке обхода (pre-order)

Изменяет только `TSourceOperator::Alias_`. Схемы пока не трогает.

```cpp
void AssignSourceAliases(const TOperatorPtr& root);
```

### Pass 2: `QualifyColumns`

Bottom-up обход. Для каждого оператора:

1. **`TSourceOperator`**: переименовать каждую колонку `col` → `alias.col`
   в своей схеме (`GetSource().Schema()`, или в `Type` напрямую).

2. **`TFilterOperator`**: обновить `TIdentExpr` в предикате —
   заменить `col` на `alias.col` по входной схеме (lookup по имени).

3. **`TJoinOperator`**: обновить `Keys_[i].Left` и `Keys_[i].Right`
   по левой и правой входным схемам.

4. **`TProjectOperator`**: обновить `TIdentExpr` в выражениях проекций;
   **выходные имена колонок** (`Projections()[i].Name`) не меняем —
   это уже новое имя, заданное пользователем. Проекция фактически
   «снимает» квалификатор: на выходе может стоять `supp_nation` (неквалифицированное).

5. **`TAggregateOperator`**: обновить `GroupKeys` (вектор string'ов) по
   входной схеме.

Вспомогательный примитив `QualifyIdent(inputSchema, name)`:
- ищет `name` в полях `TStructType` входной схемы
- возвращает квалифицированное имя (первое совпадение)
- бросает ошибку если не найдено (unresolved reference — лучше поймать здесь)

Для двоих входов (join): передаём обе схемы; имя должно быть уникальным
по объединению, иначе ошибка «ambiguous column».

```cpp
void QualifyColumns(const TOperatorPtr& root);
```

## Изменения в существующем коде

| Компонент | Что меняется |
|-----------|-------------|
| `TSourceOperator` | +`Alias_` field, +`SetAlias/GetAlias` |
| `sexp/parser.cpp` | читать опциональный второй аргумент `(rel source path alias?)` |
| `sexp/printer.cpp` | печатать алиас если задан |
| `ComputeJoinOutputType` | убрать throw на дубли (дублей после квалификации нет); но оставить assert в debug-режиме |
| `ApplyColumnPruning` | работает со строками колонок → работает с квалифицированными именами без изменений |
| `TPhysicalPlanner::Build` | без изменений |
| `bin/cli.cpp` | вызвать два новых прохода перед `AnnotateTypes` |

## Выходные колонки для пользователя

`TProjectOperator` и `TAggregateOperator` уже переименовывают колонки
(user-controlled). Финальный вывод содержит имена, заданные в sexp-е
(`supp_nation`, `cust_nation`, `revenue` и т.п.) — квалификаторов там нет.

Если в выводе нет project/agg (т.е. bare scan), выходные имена будут
квалифицированными (`nation.n_nationkey`). Для отображения можно
опционально стричь префикс в `SchemaFromType`; это можно отложить.

## Пример трансформации для Q7

До квалификации (фрагмент):
```
join
  join ... NATION[n_nationkey, n_name, ...]    ← nation_0
  NATION[n_nationkey, n_name, ...]              ← nation_1
  keys: (c_nationkey, n_nationkey)
```

После `AssignSourceAliases + QualifyColumns`:
```
join
  join ... nation_0[nation_0.n_nationkey, nation_0.n_name, ...]
  nation_1[nation_1.n_nationkey, nation_1.n_name, ...]
  keys: (c_nationkey → nation_0.c_nationkey,
         n_nationkey → nation_1.n_nationkey)
```

`ComputeJoinOutputType` видит `nation_0.n_nationkey` и `nation_1.n_nationkey`
— имена разные, конфликта нет.

## Пограничные случаи

- **Колонка уже квалифицирована** (если когда-то добавим явный `alias.col`
  в sexp): пропускать при квалификации (уже содержит `.`).
- **Computed expressions в project** (`TBinaryExpr`, `TIfExpr` и т.д.):
  `QualifyIdent` рекурсивно обходит AST выражения, заменяя только
  `TIdentExpr`-листья.
- **Self-join** (одна таблица дважды с разными алиасами): корректно
  обрабатывается нумерацией `stem_0`, `stem_1`.
- **Явный алиас в sexp**: `(rel source "__NATION__" "n1")` → алиас `n1`,
  нумерация только для auto-алиасов.

## Порядок реализации

1. `TSourceOperator` + sexp parser/printer (алиас)
2. `AssignSourceAliases` pass
3. `QualifyColumns` pass (сначала для source + join keys — хватит для Q7)
4. Убрать throw в `ComputeJoinOutputType`
5. Добавить вызовы в `cli.cpp`
6. Q7 проходит
7. Расширить `QualifyColumns` для filter/project/agg (нужно для запросов
   где эти операторы используют неквалифицированные имена из нескольких источников)

## Тесты

- Unit: `AssignSourceAliases` — две одинаковых схемы → алиасы `nation_0`/`nation_1`
- Unit: `QualifyColumns` — join keys обновляются корректно
- E2E: Q7 TPC-H проходит
- Регрессия: все 21 ранее проходящих TPC-H запросов не ломаются
