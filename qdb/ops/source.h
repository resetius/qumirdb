#pragma once

#include <qdb/ops/operator.h>

namespace NQqb {

// Wraps ISource as a relational operator.
// Converts TSchema → TStructType (stored in TExpr::Type).
class TSourceOperator : public IOperator {
public:
    static constexpr const char* OpId = "source";

    explicit TSourceOperator(ISource& source);

    std::string_view RelName() const override { return OpId; }
    ISource& GetSource() const { return Source_; }

    // Children() = {} — no inputs
    // Type = TStructType built from source's TSchema

    const std::string ToString() const override;

private:
    ISource& Source_;
};

// Builds a TStructType from TSchema.
// Useful for other operators that need to convert schema → type.
NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema);

} // namespace NQqb
