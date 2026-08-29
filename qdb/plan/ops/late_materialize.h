#pragma once

#include <qdb/plan/ops/operator.h>

#include <string>
#include <vector>

namespace NQdb {

struct TLateMaterializeColumn {
    std::string PhysicalName;
    std::string OutputName;
    NQumir::NAst::TTypePtr Type;
};

class TLateMaterializeOperator final : public IOperator {
public:
    static constexpr const char* OpId = "late-materialize";

    TLateMaterializeOperator(
        TOperatorPtr input,
        std::string locatorColumn,
        std::vector<TLateMaterializeColumn> columns);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t childIdx,
        const std::unordered_set<std::string>& needed) const override;

    std::vector<NQumir::NAst::TExprPtr> Children() const override {
        return {Input_};
    }

    const TOperatorPtr& Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    const std::string& LocatorColumn() const { return LocatorColumn_; }
    const std::vector<TLateMaterializeColumn>& Columns() const { return Columns_; }

    const std::string ToString() const override;

private:
    TOperatorPtr Input_;
    std::string LocatorColumn_;
    std::vector<TLateMaterializeColumn> Columns_;
};

} // namespace NQdb
