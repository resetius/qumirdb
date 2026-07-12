#include "estimate_stats.h"
#include "flatten_conjuncts.h"
#include "unbound_vars.h"

#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/join.h>

#include <limits>

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

    if (!operatorType) {
        return 0.5;
    }

    auto isConstExpr = [&](const TExprPtr& expr) {
        return FindUnboundVars(expr).empty();
    };

    auto columnType = [&](const std::string& colName) -> TTypePtr {
        for (auto& [col, type] : operatorType->Fields) {
            if (col == colName) {
                return type;
            }
        }
        return nullptr;
    };

    auto fractionBelow = [&](
        const std::string& colName,
        const std::shared_ptr<TNumberExpr>& num) -> std::optional<double>
    {
        auto colIt = inputStats->ColumnStats.find(colName);
        if (colIt == inputStats->ColumnStats.end()) {
            return std::nullopt;
        }
        auto type = columnType(colName);
        if (TMaybeType<TFloatType>(type)) {
            double c = num->IsFloat()
                ? num->FloatValue
                : static_cast<double>(num->IntValue);
            return colIt->second->FractionBelow(c);
        }
        if (TMaybeType<TIntegerType>(type)) {
            int64_t c = num->IsFloat()
                ? static_cast<int64_t>(num->FloatValue)
                : num->IntValue;
            return colIt->second->FractionBelow(c);
        }
        return std::nullopt;
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

        if (binary->Operator == "<" || binary->Operator == "<="
            || binary->Operator == ">" || binary->Operator == ">=") {
            const bool greater = binary->Operator == ">" || binary->Operator == ">=";
            auto leftIdent = TMaybeNode<TIdentExpr>(binary->Left);
            auto rightIdent = TMaybeNode<TIdentExpr>(binary->Right);
            auto leftNumber = TMaybeNode<TNumberExpr>(binary->Left);
            auto rightNumber = TMaybeNode<TNumberExpr>(binary->Right);
            // `col OP const`
            if (leftIdent && rightNumber) {
                if (auto f = fractionBelow(std::string(leftIdent.Cast()->Name), rightNumber.Cast())) {
                    return greater ? 1.0 - *f : *f;
                }
            }
            // `const OP col`  ==  `col (flip OP) const`
            if (rightIdent && leftNumber) {
                if (auto f = fractionBelow(std::string(rightIdent.Cast()->Name), leftNumber.Cast())) {
                    return greater ? *f : 1.0 - *f;
                }
            }
        }
    }

    return 0.5; // fallback selectivity for unknown atoms
}

TStatsPtr ComputeFilterStats(const std::shared_ptr<TFilterOperator>& filter) {
    auto inputStats = filter->Input()->Stats_;
    if (!inputStats) {
        return nullptr;
    }
    std::vector<TExprPtr> conjuncts;
    FlattenConjuncts(filter->Predicate(), conjuncts);
    double selectivity = 1.0;
    for (const auto& conjunct : conjuncts) {
        selectivity *= EstimateSelectivity(conjunct, inputStats, TMaybeType<TStructType>(filter->Input()->OutputColumns()).Cast());
    }
    selectivity = std::clamp(selectivity, 0.0, 1.0);
    auto outputStats = std::make_shared<TStats>();
    outputStats->ColumnStats = inputStats->ColumnStats;
    outputStats->RowCount = std::max<uint64_t>(1, inputStats->RowCount * selectivity);
    return outputStats;
}

TStatsPtr ComputeProjectStats(const std::shared_ptr<TProjectOperator>& project) {
    auto inputStats = project->Input()->Stats_;
    if (!inputStats) {
        return nullptr;
    }
    auto outputStats = std::make_shared<TStats>();
    outputStats->RowCount = inputStats->RowCount;
    for (const auto& proj : project->Projections()) {
        if (auto ident = TMaybeNode<TIdentExpr>(proj.Expression)) {
            auto colName = ident.Cast()->Name;
            auto it = inputStats->ColumnStats.find(colName);
            if (it != inputStats->ColumnStats.end()) {
                outputStats->ColumnStats[proj.Name] = it->second;
            }
        }
    }
    return outputStats;
}

TStatsPtr ComputeJoinStats(const std::shared_ptr<TJoinOperator>& join) {
    auto leftStats = join->Left()->Stats_;
    auto rightStats = join->Right()->Stats_;
    if (!leftStats || !rightStats) {
        return nullptr;
    }
    double ndvL = 1.0;
    double ndvR = 1.0;
    for (const auto& key : join->Keys()) {
        double leftNdv = 100.0; // unknown ndv
        double rightNdv = 100.0; // unknown ndv
        auto leftColStatsIt = leftStats->ColumnStats.find(key.Left);
        auto rightColStatsIt = rightStats->ColumnStats.find(key.Right);
        if (leftColStatsIt != leftStats->ColumnStats.end() && leftColStatsIt->second->Ndv)
        {
            leftNdv = *leftColStatsIt->second->Ndv;
        }
        if (rightColStatsIt != rightStats->ColumnStats.end() && rightColStatsIt->second->Ndv)
        {
            rightNdv = *rightColStatsIt->second->Ndv;
        }
        ndvL *= leftNdv;
        ndvR *= rightNdv;
    }
    ndvL = std::min(ndvL, (double)leftStats->RowCount);
    ndvR = std::min(ndvR, (double)rightStats->RowCount);
    double denom = std::max({ndvL, ndvR, 1.0});
    auto outputStats = std::make_shared<TStats>();

    auto jt = join->JoinType();
    if (jt == EJoinType::Inner || jt == EJoinType::Left || jt == EJoinType::Full || jt == EJoinType::LeftSemi || jt == EJoinType::LeftAnti) {
        for (const auto& [colName, colStats] : leftStats->ColumnStats) {
            // assume qualification
            outputStats->ColumnStats[colName] = colStats;
        }
    }
    if (jt == EJoinType::Inner || jt == EJoinType::Right || jt == EJoinType::Full || jt == EJoinType::RightSemi || jt == EJoinType::RightAnti) {
        for (const auto& [colName, colStats] : rightStats->ColumnStats) {
            // assume qualification
            outputStats->ColumnStats[colName] = colStats;
        }
    }
    uint64_t rowCount = std::max<uint64_t>(1, (uint64_t)((double)leftStats->RowCount * rightStats->RowCount / denom));
    switch (join->JoinType()) {
        case EJoinType::LeftSemi:
        case EJoinType::LeftAnti: {
            double semiFrac = std::min(1.0, ndvR / std::max(ndvL, 1.0));
            if (join->JoinType() == EJoinType::LeftAnti) {
                semiFrac = 1.0 - semiFrac;
            }
            rowCount = std::max<uint64_t>(1, (uint64_t)(semiFrac * (double)leftStats->RowCount));
            break;
        }
        case EJoinType::RightSemi:
        case EJoinType::RightAnti: {
            double semiFrac = std::min(1.0, ndvL / std::max(ndvR, 1.0));
            if (join->JoinType() == EJoinType::RightAnti) {
                semiFrac = 1.0 - semiFrac;
            }
            rowCount = std::max<uint64_t>(1, (uint64_t)(semiFrac * (double)rightStats->RowCount));
            break;
        }
        case EJoinType::Left:
            rowCount = std::max(rowCount, leftStats->RowCount);
            break;
        case EJoinType::Right:
            rowCount = std::max(rowCount, rightStats->RowCount);
            break;
        case EJoinType::Full:
            rowCount = std::max(rowCount, leftStats->RowCount + rightStats->RowCount);
            break;
        default:
            break;
    }
    outputStats->RowCount = rowCount;

    // Post-join ndv fixup. ColumnStats are shared_ptr aliased with the child
    // (and ultimately the source) stats, so copy-on-write before touching Ndv —
    // never mutate the shared objects in place.
    auto capNdv = [](TStats::TColumnStatsPtr& slot, uint64_t cap) {
        if (!slot || !slot->Ndv || *slot->Ndv <= cap) {
            return; // only shrink a known, too-large ndv
        }
        auto copy = std::make_shared<TStats::TColumnStats>(*slot);
        copy->Ndv = cap;
        slot = std::move(copy);
    };
    // A column can't have more distinct values than the surviving rows.
    for (auto& [name, slot] : outputStats->ColumnStats) {
        capNdv(slot, rowCount);
    }
    // Equi-join keys collapse to their matched distinct count ~ min(ndvL, ndvR).
    auto ndvOf = [](const TStatsPtr& s, const std::string& col) {
        auto it = s->ColumnStats.find(col);
        return (it != s->ColumnStats.end() && it->second->Ndv)
            ? *it->second->Ndv : std::numeric_limits<uint64_t>::max();
    };
    for (const auto& key : join->Keys()) {
        uint64_t keyNdv = std::min(ndvOf(leftStats, key.Left), ndvOf(rightStats, key.Right));
        if (auto it = outputStats->ColumnStats.find(key.Left); it != outputStats->ColumnStats.end()) {
            capNdv(it->second, keyNdv);
        }
        if (auto it = outputStats->ColumnStats.find(key.Right); it != outputStats->ColumnStats.end()) {
            capNdv(it->second, keyNdv);
        }
    }
    return outputStats;
}

TStatsPtr ComputeSourceStats(const std::shared_ptr<TSourceOperator>& source) {
    auto raw = source->GetSource().Stats();
    if (!raw) {
        return nullptr;
    }
    const std::string& alias = source->GetAlias();
    auto out = std::make_shared<TStats>();
    out->RowCount = raw->RowCount;
    for (const auto& col : source->GetSource().Schema().Columns) {
        auto it = raw->ColumnStats.find(std::string(col.Name));
        if (it != raw->ColumnStats.end()) {
            out->ColumnStats[alias + "." + std::string(col.Name)] = it->second;
        }
    }
    return out;
}

TStatsPtr ComputeStatsFor(TOperatorPtr op) {
    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        return ComputeSourceStats(source.Cast());
    }

    if (auto maybeFilter = TMaybeOp<TFilterOperator>(op)) {
        return ComputeFilterStats(maybeFilter.Cast());
    }
    if (auto maybeProject = TMaybeOp<TProjectOperator>(op)) {
        return ComputeProjectStats(maybeProject.Cast());
    }
    if (auto maybeJoin = TMaybeOp<TJoinOperator>(op)) {
        return ComputeJoinStats(maybeJoin.Cast());
    }

    return nullptr; // TODO: aggregate / sort / limit
}

} // namespace

TStatsPtr EstimateStats(TOperatorPtr op) {
    for (auto& child : op->Children()) {
        if (auto maybeOp = TMaybeNode<IOperator>(child)) {
            auto childOp = maybeOp.Cast();
            childOp->Stats_ = EstimateStats(childOp);
        }
    }

    return op->Stats_ = ComputeStatsFor(op);
}

} // namespace NQdb