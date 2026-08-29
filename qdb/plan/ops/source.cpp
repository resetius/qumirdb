#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/type.h>

#include <functional>

namespace NQdb {

namespace {

NQumir::NAst::TTypePtr SourceOutputType(
    const ISource& source,
    const std::string& alias,
    bool emitRowId)
{
    using namespace NQumir::NAst;
    std::vector<std::pair<std::string, TTypePtr>> fields;
    fields.reserve(source.Schema().Columns.size() + (emitRowId ? 1 : 0));
    for (const auto& col : source.Schema().Columns) {
        auto name = std::string(col.Name);
        fields.emplace_back(alias.empty() ? name : alias + "." + name, col.Type);
    }
    if (emitRowId) {
        const std::string rowIdName(InternalRowIdColumnName);
        fields.emplace_back(
            alias.empty() ? rowIdName : alias + "." + rowIdName,
            std::make_shared<TIntegerType>(TIntegerType::U64));
    }
    return std::make_shared<TStructType>(std::move(fields));
}

} // namespace

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
    if (auto* fun = static_cast<NQumir::NAst::TFunctionType*>(Type.get())) {
        fun->ReturnType = SourceOutputType(Source_, Alias_, EmitRowId_);
    }
}

void TSourceOperator::EnableRowId() {
    EmitRowId_ = true;
    if (auto* fun = static_cast<NQumir::NAst::TFunctionType*>(Type.get())) {
        fun->ReturnType = SourceOutputType(Source_, Alias_, true);
    }
}

std::string TSourceOperator::RowIdColumn() const {
    const std::string rowIdName(InternalRowIdColumnName);
    return Alias_.empty() ? rowIdName : Alias_ + "." + rowIdName;
}

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

} // namespace NQdb
