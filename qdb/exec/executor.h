#pragma once

#include <qdb/io/io.h>

#include <qumir/parser/type.h>

namespace NQdb {

struct IRuntimeNode {
    virtual ~IRuntimeNode() = default;
    virtual NQumir::NAst::TTypePtr OutputType() const = 0; // TStructType
    virtual bool Next(TRowSet& rowSet) = 0;
};

} // namespace NQdb
