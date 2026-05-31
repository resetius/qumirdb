#pragma once

#include <qdb/io/io.h>

#include <ostream>
#include <string>
#include <vector>

namespace NQqb {

// PostgreSQL-style aligned table output.
// Buffers all rows; call Flush() (or destroy) to print.
class TConsoleSink : public ISink {
public:
    TConsoleSink(const TSchema& schema, std::ostream& out);

    void Write(const TRowSet& rowSet) override;
    void Flush() override;

private:
    const TSchema& Schema_;
    std::ostream& Out_;
    std::vector<std::vector<std::string>> Rows_;
    std::vector<size_t> Widths_;
};

// CSV output with configurable separator.
class TCsvSink : public ISink {
public:
    TCsvSink(const TSchema& schema, std::ostream& out, char separator = ',');

    void Write(const TRowSet& rowSet) override;

private:
    const TSchema& Schema_;
    std::ostream& Out_;
    char Separator_;
    bool HeaderPrinted_ = false;
};

} // namespace NQqb
