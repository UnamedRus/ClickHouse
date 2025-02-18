---
slug: /ru/sql-reference/aggregate-functions/reference/kolmogorovsmirnovtest
sidebar_position: 300
sidebar_label: kolmogorovSmirnovTest
---

# kolmogorovSmirnovTest {#kolmogorovSmirnovTest}

Проводит статистический тест Колмогорова-Смирнова для двух независимых выборок.

**Синтаксис**

``` sql
kolmogorovSmirnovTest([alternative, computation_method])(sample_data, sample_index)
```

Значения выборок берутся из столбца `sample_data`. Если  `sample_index` равно 0, то значение из этой строки принадлежит первой выборке. Во всех остальных случаях значение принадлежит второй выборке.
Выборки должны принадлежать непрерывным одномерным распределениям.

**Аргументы**

-   `sample_data` — данные выборок. [Integer](../../../sql-reference/data-types/int-uint.md), [Float](../../../sql-reference/data-types/float.md) or [Decimal](../../../sql-reference/data-types/decimal.md).
-   `sample_index` — индексы выборок. [Integer](../../../sql-reference/data-types/int-uint.md).

**Параметры**

- `alternative` — альтернативная гипотеза (Необязательный параметр, по умолчанию: `'two-sided'`.) [String](../../../sql-reference/data-types/string.md).
    Пусть `F(x) и G(x)` - функции распределения первой и второй выборки соотвественно.
    - `'two-sided'`
        Нулевая гипотеза состоит в том, что выборки происходит из одного и того же распределение, то есть `F(x) = G(x)` для любого x.
        Альтернатива - выборки принадлежат разным распределениям.
    - `'greater'`
        Нулевая гипотеза состоит в том, что элементы первой выборки в асимптотически почти наверное меньше элементов из второй выборки,
        то есть функция распределения первой выборки лежит выше и соотвественно левее, чем функция распределения второй выборки.
        Таким образом это означает, что `F(x) >= G(x)` for любого x, а альтернатива в этом случае состоит в том, что `F(x) < G(x)` хотя бы для одного x.
    - `'less'`.
        Нулевая гипотеза состоит в том, что элементы первой выборки в асимптотически почти наверное больше элементов из второй выборки,
        то есть функция распределения первой выборки лежит ниже и соотвественно правее, чем функция распределения второй выборки.
        Таким образом это означает, что `F(x) <= G(x)` for любого x, а альтернатива в этом случае состоит в том, что `F(x) > G(x)` хотя бы для одного x.
- `computation_method` — метод, используемый для вычисления p-value. (Необязательный параметр, по умолчанию: `'auto'`.) [String](../../../sql-reference/data-types/string.md).
    - `'exact'` - вычисление производится с помощью вычисления точного распределения статистики. Требует большого количества вычислительных ресурсов и расточительно для больших выборок.
    - `'asymp'`(`'asymptotic'`) - используется приближенное вычисление. Для больших выборок приближенный результат и точный почти идентичны.
    - `'auto'`  - значение вычисляется точно (с помощью метода `'exact'`), если максимальный размер двух выборок не превышает 10'000.

**Возвращаемые значения**

[Кортеж](../../../sql-reference/data-types/tuple.md) с двумя элементами:

-   вычисленное статистики. [Float64](../../../sql-reference/data-types/float.md).
-   вычисленное p-value. [Float64](../../../sql-reference/data-types/float.md).


**Пример**

Запрос:

``` sql
SELECT kolmogorovSmirnovTest('less', 'exact')(value, num)
FROM
(
    SELECT
        randNormal(0, 10) AS value,
        0 AS num
    FROM numbers(10000)
    UNION ALL
    SELECT
        randNormal(0, 10) AS value,
        1 AS num
    FROM numbers(10000)
)
```

Результат:

``` text
┌─kolmogorovSmirnovTest('less', 'exact')(value, num)─┐
│ (0.009899999999999996,0.37528595205132287)         │
└────────────────────────────────────────────────────┘
```

Заметки:
P-value больше чем 0.05 (для уровня значимости 95%), то есть нулевая гипотеза не отвергается.


Запрос:

``` sql
SELECT kolmogorovSmirnovTest('two-sided', 'exact')(value, num)
FROM
(
    SELECT
        randStudentT(10) AS value,
        0 AS num
    FROM numbers(100)
    UNION ALL
    SELECT
        randNormal(0, 10) AS value,
        1 AS num
    FROM numbers(100)
)
```

Результат:

``` text
┌─kolmogorovSmirnovTest('two-sided', 'exact')(value, num)─┐
│ (0.4100000000000002,6.61735760482795e-8)                │
└─────────────────────────────────────────────────────────┘
```

Заметки:
P-value меньше чем 0.05 (для уровня значимости 95%), то есть нулевая гипотеза отвергается.


**Смотрите также**

- [Критерий согласия Колмогорова-Смирнова](https://ru.wikipedia.org/wiki/%D0%9A%D1%80%D0%B8%D1%82%D0%B5%D1%80%D0%B8%D0%B9_%D1%81%D0%BE%D0%B3%D0%BB%D0%B0%D1%81%D0%B8%D1%8F_%D0%9A%D0%BE%D0%BB%D0%BC%D0%BE%D0%B3%D0%BE%D1%80%D0%BE%D0%B2%D0%B0)
