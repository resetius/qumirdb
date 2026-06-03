#pragma once

#include <qdb/ops/operator.h>

#include <unordered_set>
#include <string>

namespace NQqb {

class TSourceOperator : public IOperator {
public:
    static constexpr const char* OpId = "source";

    explicit TSourceOperator(ISource& source, std::string path = {});

    std::string_view RelName() const override { return OpId; }
    ISource& GetSource() const { return Source_; }
    const std::string& SourcePath() const { return SourcePath_; }

    const std::unordered_set<std::string>& RequiredColumns() const { return RequiredColumns_; }
    void SetRequiredColumns(std::unordered_set<std::string> cols) { RequiredColumns_ = std::move(cols); }

    const std::string ToString() const override;

private:
    ISource& Source_;
    std::string SourcePath_;
    std::unordered_set<std::string> RequiredColumns_; // empty = all columns
};

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema);

// Transformation pass: walks the plan tree, collects used columns via
// CollectUsedColumns, and calls SetRequiredColumns on all TSourceOperator nodes.
void ApplyColumnPruning(const TOperatorPtr& root);

} // namespace NQqb
