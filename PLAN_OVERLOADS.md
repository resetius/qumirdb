# Plan: Polymorphic Function Dispatch in Qumir Core

## Цель

Разрешить несколько функций с одним именем и разными сигнатурами — как для
внутренних (`fun`), так и для внешних (модульных). При вызове выбирать лучший
кандидат, вставлять implicit cast'ы для widening-конверсий.

Тестируется напрямую в qumir-программах:
```
fun add(x i32, y i32) i32 (+ x y)
fun add(x f64, y f64) f64 (+ x f64)
; (add 1 2)   → i32 версия
; (add 1.0 2.0) → f64 версия
```

---

## Текущее состояние

| Механизм | Где | Состояние |
|---|---|---|
| Binary/unary op overloads | `ImportedBinaryOps` / `ImportedUnaryOps` | Exact match; "two-level cast" в type annotator |
| Named function overloads | — | Нет; повторное имя → ошибка redeclaration |
| Implicit casts | `ImportedCasts` | Есть, но только явные |
| Widening в resolution | type annotator | Частично для binary ops |

---

## Как разрешение прорастает в IR

Паттерн уже есть для операторов: каждый оператор при регистрации получает
**synthetic name** (`__binop_=_str_view_str_view`), объявляется через
`DeclareFunction(synthName, funDecl)`, а type annotator при разрешении
создаёт `TCallExpr` с `callee = TIdentExpr(synthName)` через `MakeModuleOpCall`.
IR видит уже мономорфное уникальное имя — никакой двусмысленности.

Для named overloads — тот же принцип:

```
fun add(x i32, y i32) i32  →  synthName = "__overload_add_i32_i32"
fun add(x f64, y f64) f64  →  synthName = "__overload_add_f64_f64"
```

- Name resolver: `DeclareFunction(synthName, funDecl)` для каждого варианта;
  каноническое имя `"add"` → `FunctionOverloadSets["add"]` → список synthName'ов.
- Type annotator: после выбора победителя переписывает вызов через `MakeModuleOpCall(synthName, ...)`.
- IR lowering: видит `TCallExpr` с уникальным именем — изменений не требует.

Разрешение происходит **ровно один раз** — в type annotator. После него AST монморфный.

---

## Шаг 1 — Name resolver: overload sets вместо single symbol

**Файл:** `qumir/semantics/name_resolution/name_resolver.h/.cpp`

Сейчас `Declare` / `DeclareFunction` при повторном имени выдаёт ошибку.

Изменить: функции (`TFunDecl` и `TExternalFunction`) складываются в overload set
по имени. Не-функциям (переменные, типы) повторное имя по-прежнему запрещено.

```cpp
// Вместо NameToSymbolId для функций — список кандидатов
std::unordered_map<std::string, std::vector<TSymbolId>> FunctionOverloadSets;
```

`Lookup(name)` для функциональных имён возвращает не один `TSymbolId`,
а весь overload set — type annotator выбирает нужный.

Добавить метод:
```cpp
std::vector<TSymbolId> LookupOverloads(const std::string& name, TScopeId scope) const;
```

---

## Шаг 2 — Таблица implicit widening коерций

**Файл:** новый `qumir/semantics/coercions.h`

Widening — участвуют в overload resolution бесплатно (cost = 1 на шаг):

```
i8  → i16 → i32 → i64
u8  → u16 → u32 → u64
u8  → i16,  u16 → i32,  u32 → i64   (unsigned → signed)
f32 → f64
i32 → f64
```

Не implicit: narrowing, пользовательские named types, `string → str_view`.

```cpp
// Число шагов widening from → to, или nullopt если нет пути.
std::optional<int> WideningCost(const TTypePtr& from, const TTypePtr& to);
```

---

## Шаг 3 — Overload resolution в `TTypeAnnotator`

**Файл:** `qumir/semantics/type_annotation/type_annotation.cpp`

При аннотации `TCallExpr`:

```
1. LookupOverloads(name) → список кандидатов.
   Если один — текущий путь (exact, без изменений).
   Если ноль — ошибка "unknown function".

2. Для каждого кандидата:
   a. Проверить число аргументов
   b. Для каждой пары (arg_type, param_type):
      - exact match          → cost 0
      - WideningCost(a, p)   → cost N
      - нет пути             → кандидат отбрасывается
   c. total_cost = сумма

3. Минимальный total_cost → победитель.
   Несколько с равным минимумом → ошибка ambiguous.

4. Для аргументов с cost > 0: обернуть в TCastExpr(arg, param_type).

5. Подставить в TCallExpr тип и ссылку на выбранный кандидат.
```

Заменить этим же алгоритмом текущий "two-level cast" для binary/unary ops —
убрать дублирование.

---

## Шаг 4 — Тесты в qumir regtest

Добавить regtest-кейсы в `qumir/test/regtest/`:

```
; overload_basic: две функции add(i32,i32) и add(f64,f64)
; overload_widening: вызов с i32 выбирает i64 версию через widening
; overload_ambiguous: две версии с равной стоимостью → compile error
; overload_external: модульная функция перегружает внутреннюю или наоборот
```

---

## Шаг 5 — QumirDb модуль: `str_view` + перегрузки

**Файл:** `qdb/modules/qumirdb.cpp`

После шагов 1–4:

```cpp
// Тип
{ .Name = "str_view",
  .Type = struct{ ptr: <ptr u8>, len: i32 } }

// Перегрузка = для str_view (IsOp = true, повторное имя разрешено как overload)
{ .Name = "=", .IsOp = true,
  .ArgTypes = {str_view, str_view}, .ReturnType = bool,
  .Inline = /* побайтовое сравнение */ }

// str_hash
{ .Name = "str_hash",
  .ArgTypes = {str_view}, .ReturnType = u64,
  .Inline = /* FNV-1a */ }
```

---

## Шаг 6 — Kernel codegen: STRING колонки → `str_view`

**Файл:** `qdb/kernel/gen.cpp`

Для `TStringType` колонок вместо прямого `(index i Data)`:
```
(let ((sv_ptr (cast (+ (field Data col)
                       (index i (cast (field Offsets col) <ptr i32>)))
                    <ptr u8>))
     (sv_len  (- (index (+ i 1) (cast (field Offsets col) <ptr i32>))
                 (index i (cast (field Offsets col) <ptr i32>)))))
  (struct ((ptr sv_ptr) (len sv_len))))
```
Тип параметра в kernel signature: `str_view`.

---

## Организация кода

```
qumir/semantics/
  name_resolution/
    name_resolver.h/.cpp   ← FunctionOverloadSets, LookupOverloads()
                             Логика хранения — только здесь. Без выбора кандидата.
    symbol_info.h
  type_annotation/
    type_annotation.h/.cpp ← алгоритм resolution, вставка TCastExpr
                             Заменяет текущий "two-level cast" для ops.
    coercions.h/.cpp       ← новый: WideningCost(), таблица widening.
                             Пока используется только type annotator'ом;
                             вынесем на уровень semantics/ если понадобится шире.
  definite_assignment/
  transform/
```

Принципы разделения:
- **Name resolver** — только хранит overload sets. Не знает про типы аргументов вызова.
- **Type annotator** — единственное место выбора кандидата. Знает типы, вставляет cast'ы.
- **coercions** — чистая утилита без зависимостей на AST-обход.

---

## Порядок реализации

1. `coercions.h/.cpp` — таблица widening (изолированно, легко тестировать)
2. Name resolver: overload sets для функций
3. Type annotator: overload resolution алгоритм; унифицировать с текущим op-resolution
4. Regtest кейсы в qumir
5. `str_view` в QumirDb + тесты
6. Kernel codegen для STRING колонок
