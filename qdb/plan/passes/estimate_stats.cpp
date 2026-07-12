#include "estimate_stats.h"
#include "flatten_conjuncts.h"
#include "unbound_vars.h"

#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/filter.h>

namespace NQdb {

using namespace NQumir::NAst;

/*

  Селективность одного конъюнкта (тут окупается гистограмма):
  - col = const → 1/ndv; если const ∉ [min,max] → 0. Точнее — по бакету гистограммы.
  - col < / <= / > / >= диапазон → equi-depth гистограмма: доля = (кол-во границ по нужную сторону)/N с интерполяцией в «граничном» бакете. Нет гистограммы → линейно по (const-min)/(max-min).
  - col BETWEEN a AND b (или два range-конъюнкта на одну колонку) → доля гистограммы в [a,b].
  - col IS NULL → null_count/RowCount; IS NOT NULL → 1 - ….
  - col IN (…) → min(1, |list|/ndv).
  - col1 = col2 (residual) → 1/max(ndv1, ndv2).
  min(ndv, RowCount).


  ┌─────────────┬─────────────────────────────────────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────────────┐
  │  Оператор   │                    RowCount                     │                                                колонки                                                │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Source      │ из футера (TParquetSource::Stats())             │ min/max/null/ndv/hist                                                                                 │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Project     │ без изменений                                   │ прямые ссылки — pass-through; производные (a*b, func) — stats теряются                                │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Join (equi) │ |L|·|R| / max(ndv_L.key, ndv_R.key)             │ ключ: ndv=min, min/max = пересечение; остальное pass-through, ndv≤RowCount. Semi/anti — зажать по |L| │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Aggregate   │ Π ndv(group-key), cap на вход; без group by → 1 │ group-key: ndv/min/max сохраняются; агрегаты — min/max неизвестны (кроме min()/max()/count)           │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Sort        │ pass-through                                    │ pass-through                                                                                          │
  ├─────────────┼─────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────┤
  │ Limit       │ min(RowCount, N)                                │ pass-through                                                                                          │
  └─────────────┴─────────────────────────────────────────────────┴───────────────────────────────────────────────────────────────────────────────────────────────────────┘
*/

namespace {

double EstimateSelectivity(const TExprPtr& atom, TStatsPtr inputStats, std::shared_ptr<TStructType> operatorType) {
    // TODO: eval consts

    auto isConstExpr = [&](const TExprPtr& expr) {
        return FindUnboundVars(expr).empty();
    };

    auto inRange = [&](TExprPtr expr, const std::string& colName) -> std::optional<bool> {
        if (inputStats->ColumnStats.count(colName) == 0) {
            return std::nullopt;
        }
        auto colStats = inputStats->ColumnStats[colName];
        if (!colStats->MinValue || !colStats->MaxValue) {
            return std::nullopt;
        }
        auto maybeNumber = TMaybeNode<TNumberExpr>(expr);
        if (!maybeNumber) {
            return std::nullopt;
        }
        bool isIntegerType = false;
        bool isSigned = false;
        bool isFloatType = false;
        for (auto& [col, type] : operatorType->Fields) {
            if (col == colName) {
                if (auto integerType = TMaybeType<TIntegerType>(type)) {
                    isIntegerType = true;
                    isSigned = integerType.Cast()->IsSigned();
                } else if (TMaybeType<TFloatType>(type)) {
                    isFloatType = true;
                } else {
                    return std::nullopt;
                }
                break;
            }
        }
        auto number = maybeNumber.Cast();
        if (isIntegerType) {
            if (number->IsFloat()) {
                if (isSigned) {
                    return *colStats->GetMinValue<int64_t>() <= number->FloatValue
                        && number->FloatValue <= colStats->GetMaxValue<int64_t>();
                } else {
                    return *colStats->GetMinValue<uint64_t>() <= number->FloatValue
                        && number->FloatValue <= colStats->GetMaxValue<uint64_t>();
                }
            } else {
                if (isSigned) {
                    return *colStats->GetMinValue<int64_t>() <= number->IntValue
                        && number->IntValue <= colStats->GetMaxValue<int64_t>();
                } else {
                    return *colStats->GetMinValue<uint64_t>() <= number->IntValue
                        && number->IntValue <= colStats->GetMaxValue<uint64_t>();
                }
            }
        } else {
            // float
            // TODO: only double supported
            if (number->IsFloat()) {
                return *colStats->GetMinValue<double>() <= number->FloatValue
                    && number->FloatValue <= colStats->GetMaxValue<double>();
            } else {
                return *colStats->GetMinValue<double>() <= number->IntValue
                    && number->IntValue <= colStats->GetMaxValue<double>();
            }
        }
    };

    if (auto maybeBinary = TMaybeNode<TBinaryExpr>(atom)) {
        auto binary = maybeBinary.Cast();
        if (binary->Operator == "==" || binary->Operator == "!=") {
            double selectivity = 0.5;
            auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left);
            auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right);
            if (leftIdent && rightIdent) {
                auto leftName = std::string(leftIdent.Cast()->Name);
                auto rightName = std::string(rightIdent.Cast()->Name);
                if (inputStats->ColumnStats.count(leftName) > 0
                    && inputStats->ColumnStats.count(rightName) > 0)
                {
                    auto leftNdv = inputStats->ColumnStats[leftName]->Ndv;
                    auto rightNdv = inputStats->ColumnStats[rightName]->Ndv;
                    if (leftNdv && rightNdv) {
                        selectivity = 1.0 / std::max(*leftNdv, *rightNdv);
                    }
                }
            } else if (leftIdent && isConstExpr(binary->Right)) {
                auto leftName = std::string(leftIdent.Cast()->Name);
                if (inputStats->ColumnStats.count(leftName) > 0) {
                    auto inRangeResult = inRange(binary->Right, leftName);
                    if (inRangeResult.has_value() && !*inRangeResult) {
                        selectivity = 0.0;
                    } else {
                        auto leftNdv = inputStats->ColumnStats[leftName]->Ndv;
                        if (leftNdv) {
                            selectivity = 1.0 / *leftNdv;
                        }
                    }
                }
            } else if (rightIdent && isConstExpr(binary->Left)) {
                auto rightName = std::string(rightIdent.Cast()->Name);
                if (inputStats->ColumnStats.count(rightName) > 0) {
                    auto inRangeResult = inRange(binary->Left, rightName);
                    if (inRangeResult.has_value() && !*inRangeResult) {
                        selectivity = 0.0;
                    } else {
                        auto rightNdv = inputStats->ColumnStats[rightName]->Ndv;
                        if (rightNdv) {
                            selectivity = 1.0 / *rightNdv;
                        }
                    }
                }
            }
            if (binary->Operator == "!=") {
                selectivity = 1.0 - selectivity;
            }
            return selectivity;
        } // ==, !=

        if (binary->Operator == '<' || binary->Operator == "<=" || binary->Operator == '>' || binary->Operator == ">=") {
            auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left);
            auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right);
            auto leftNumber = TMaybeNode<TNumberExpr>(binary->Left);
            auto rightNumber = TMaybeNode<TNumberExpr>(binary->Right);
            if (leftIdent && rightNumber && inputStats->ColumnStats.count(leftIdent.Cast()->Name) > 0) {
                auto leftName = leftIdent.Cast()->Name;
                auto number = rightNumber.Cast();
                auto fraction = inputStats->ColumnStats[leftName]->FractionBelow(number->IsFloat() ? number->FloatValue : number->IntValue);
                if (fraction) {
                    return (binary->Operator == '>' || binary->Operator == ">=")
                        ? 1.0 - *fraction
                        : *fraction;
                }
            }
            if (rightIdent && leftNumber && inputStats->ColumnStats.count(rightIdent.Cast()->Name) > 0) {
                auto rightName = rightIdent.Cast()->Name;
                auto number = leftNumber.Cast();
                auto fraction = inputStats->ColumnStats[rightName]->FractionBelow(number->IsFloat() ? number->FloatValue : number->IntValue);
                if (fraction) {
                    return (binary->Operator == '<' || binary->Operator == "<=")
                        ? 1.0 - *fraction
                        : *fraction;
                }
            }
        }
    }

    return 0.5; // fallback selectivity for unknown atoms
}

TStatsPtr ComputeFilterStats(const std::shared_ptr<TFilterOperator>& filter) {
    auto inputStats = filter->Input()->Stats_;
    std::vector<TExprPtr> conjuncts;
    FlattenConjuncts(filter->Predicate(), conjuncts);
    double selectivity = 1.0;
    for (const auto& conjunct : conjuncts) {
        selectivity *= EstimateSelectivity(conjunct, inputStats, TMaybeType<TStructType>(filter->Type).Cast());
    }
    auto outputStats = std::make_shared<TStats>();
    outputStats->ColumnStats = inputStats->ColumnStats;
    outputStats->RowCount = std::max<uint64_t>(1, inputStats->RowCount * selectivity);
    return outputStats;
}

TStatsPtr ComputeStatsFor(TOperatorPtr op) {
    if (op->Stats_) {
        return op->Stats_; // do we need to recompute?
    }

    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        return source.Cast()->Stats_;
    }

    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        return ComputeFilterStats(maybeFilter.Cast());
    }

    return nullptr; // TODO
}

} // namespace

TStatsPtr EstimateStats(TOperatorPtr op) {
    for (auto& child : op->Children()) {
        if (auto maybeOp = TMaybeNode<IOperator>(child)) {
            auto childOp = maybeOp.Cast();
            childOp->Stats_ = EstimateStats(childOp);
        }
    }

    return op->Stats_;
}

} // namespace NQdb