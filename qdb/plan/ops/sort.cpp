#include <qdb/plan/ops/sort.h>

#include <qumir/parser/type.h>

#include <memory>
#include <utility>

namespace NQdb {

using namespace NQumir::NAst;

std::string_view SortDirectionName(ESortDirection direction) {
    switch (direction) {
        case ESortDirection::Asc:
            return "asc";
        case ESortDirection::Desc:
            return "desc";
    }
    return "asc";
}

std::string_view SortNullsName(ESortNulls nulls) {
    switch (nulls) {
        case ESortNulls::Default:
            return "nulls-default";
        case ESortNulls::First:
            return "nulls-first";
        case ESortNulls::Last:
            return "nulls-last";
    }
    return "nulls-default";
}

TSortOperator::TSortOperator(TOperatorPtr input, std::vector<TSortKey> keys)
    : Input_(std::move(input))
    , Keys_(std::move(keys))
{
    auto inputSchema = Input_->OutputColumns();
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{inputSchema},
        inputSchema);
}

std::unordered_set<std::string> TSortOperator::ComputeReferencedColumns() const {
    std::unordered_set<std::string> refs;
    for (const auto& key : Keys_) {
        refs.insert(key.Column);
    }
    return refs;
}

const std::string TSortOperator::ToString() const {
    std::string s = "(rel sort " + Input_->ToString();
    for (const auto& key : Keys_) {
        s += " (" + key.Column + " ";
        s += SortDirectionName(key.Direction);
        s += " ";
        s += SortNullsName(key.Nulls);
        s += ")";
    }
    return s + ")";
}

} // namespace NQdb
