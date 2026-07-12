#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/type.h>

#include <functional>

namespace NQdb {

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema) {
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    fields.reserve(schema.Columns.size());
    for (const auto& col : schema.Columns) {
        fields.emplace_back(std::string(col.Name), col.Type);
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

TSourceOperator::TSourceOperator(ISource& source, std::string path)
    : Source_(source)
    , SourcePath_(std::move(path))
{
    Type = std::make_shared<NQumir::NAst::TFunctionType>(
        std::vector<NQumir::NAst::TTypePtr>{},
        StructTypeFromSchema(source.Schema()));
    const std::string& alias = GetAlias();
    Stats_ = std::make_shared<TStats>();
    Stats_->RowCount = source.Stats()->RowCount;
    auto sourceStats = source.Stats();
    for (const auto& col : source.Schema().Columns) {
        auto qualified = alias + "." + std::string(col.Name);
        auto it = sourceStats->ColumnStats.find(std::string(col.Name));
        if (it != sourceStats->ColumnStats.end()) {
            Stats_->ColumnStats[qualified] = it->second;
        }
    }
}

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

} // namespace NQdb
