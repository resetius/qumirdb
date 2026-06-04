#include <qdb/ops/source.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>
#include <qdb/pipeline/unbound_vars.h>

#include <qumir/parser/type.h>

#include <functional>

namespace NQqb {

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

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

} // namespace NQqb
