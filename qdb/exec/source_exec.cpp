#include <qdb/exec/source_exec.h>

namespace NQdb {

TRuntimeSource::TRuntimeSource(ISource& source, NQumir::NAst::TTypePtr type)
    : Source_(source)
    , Type_(std::move(type))
{}

bool TRuntimeSource::Next(TRowSet& rowSet) {
    return Source_.Next(rowSet);
}

} // namespace NQdb
