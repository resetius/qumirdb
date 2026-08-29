#include <qdb/plan/ops/late_materialize.h>

#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <utility>

namespace NQdb {

TLateMaterializeOperator::TLateMaterializeOperator(
    TOperatorPtr input,
    std::string locatorColumn,
    std::vector<TLateMaterializeColumn> columns)
    : Input_(std::move(input))
    , LocatorColumn_(std::move(locatorColumn))
    , Columns_(std::move(columns))
{
    using namespace NQumir::NAst;
    std::vector<std::pair<std::string, TTypePtr>> fields;
    fields.reserve(Columns_.size());
    for (const auto& column : Columns_) {
        fields.emplace_back(column.OutputName, column.Type);
    }
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{Input_->OutputColumns()},
        std::make_shared<TStructType>(std::move(fields)));
}

std::unordered_set<std::string>
TLateMaterializeOperator::ComputeReferencedColumns() const {
    return {LocatorColumn_};
}

std::unordered_set<std::string> TLateMaterializeOperator::RequiredColumnsForChild(
    size_t,
    const std::unordered_set<std::string>&) const
{
    return {LocatorColumn_};
}

const std::string TLateMaterializeOperator::ToString() const {
    using NQumir::NAst::NCore::PrintType;

    std::string result = "(rel late-materialize " + Input_->ToString() +
        " (locator " + LocatorColumn_ + ")";
    for (const auto& column : Columns_) {
        result += " (column " + column.OutputName + " " +
            column.PhysicalName + " " + PrintType(column.Type) + ")";
    }
    return result + ")";
}

} // namespace NQdb
