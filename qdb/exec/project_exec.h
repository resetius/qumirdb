#pragma once

#include <qdb/exec/executor.h>

#include <memory>
#include <vector>

namespace NQqb {

class TRuntimeProject : public IRuntimeNode {
public:
    TRuntimeProject(
        std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        std::vector<int32_t> columnIndices);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    std::vector<int32_t> ColumnIndices_;
};

} // namespace NQqb
