#include <qdb/ops/source.h>

#include <qumir/parser/type.h>

namespace NQqb {

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema) {
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    fields.reserve(schema.Columns.size());
    for (const auto& col : schema.Columns) {
        fields.emplace_back(std::string(col.Name), col.Type);
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

TSourceOperator::TSourceOperator(ISource& source)
    : Source_(source)
{
    Type = StructTypeFromSchema(source.Schema());
}

const std::string TSourceOperator::ToString() const {
    return "(rel source)";
}

} // namespace NQqb
