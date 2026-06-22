#pragma once

#include <qdb/plan/ops/operator.h>

#include <unordered_set>
#include <string>

namespace NQqb {

class TSourceOperator : public IOperator {
public:
    static constexpr const char* OpId = "source";

    explicit TSourceOperator(ISource& source, std::string path = {});

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override { return {}; }
    ISource& GetSource() const { return Source_; }
    const std::string& SourcePath() const { return SourcePath_; }
    const std::string& GetAlias() const { return Alias_; }
    void SetAlias(std::string alias) { Alias_ = std::move(alias); }

    const std::string ToString() const override;

private:
    ISource& Source_;
    std::string SourcePath_;
    std::string Alias_;
};

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema);

} // namespace NQqb
