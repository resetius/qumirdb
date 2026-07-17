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
}

void TSourceOperator::SetAlias(std::string alias) {
    Alias_ = std::move(alias);
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    for (const auto& col : Source_.Schema().Columns) {
        fields.emplace_back(Alias_ + "." + std::string(col.Name), col.Type);
    }
    auto qStruct = std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
    if (auto* fun = static_cast<NQumir::NAst::TFunctionType*>(Type.get())) {
        fun->ReturnType = std::move(qStruct);
    }
}

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

} // namespace NQdb
