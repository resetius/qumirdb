#pragma once

#include <qdb/exec/executor.h>
#include <qdb/io/io.h>

namespace NQdb {

class TRuntimeSource : public IRuntimeNode {
public:
    TRuntimeSource(ISource& source, NQumir::NAst::TTypePtr type);

    NQumir::NAst::TTypePtr OutputType() const override { return Type_; }
    bool Next(TRowSet& rowSet) override;

private:
    ISource& Source_;
    NQumir::NAst::TTypePtr Type_;
};

} // namespace NQdb
