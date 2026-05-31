#include <qdb/io/text/sink.h>

#include <format>
#include <iomanip>
#include <string_view>
#include <string>

#include <qumir/parser/type.h>

namespace NQqb {

namespace {

using namespace NQumir::NAst;

bool IsMasked(const TColumn& col, int32_t row) {
    if (!col.Mask) {
        return false;
    }
    const int64_t bitIndex = static_cast<int64_t>(col.MaskBitOffset) + row;
    const uint8_t byte = col.Mask[bitIndex >> 3];
    return ((byte >> (bitIndex & 7)) & 1) == 0;
}

bool IsBitSet(const TColumn& col, int32_t row) {
    const auto* bits = reinterpret_cast<const uint8_t*>(col.Data);
    const int64_t bitIndex = static_cast<int64_t>(col.DataBitOffset) + row;
    return ((bits[bitIndex >> 3] >> (bitIndex & 7)) & 1) != 0;
}

template <typename TOffset>
std::string_view StringViewValue(const TColumn& col, int32_t row) {
    const auto* offsets = static_cast<const TOffset*>(col.Offsets);
    const TOffset base = offsets[0];
    const TOffset start = offsets[row] - base;
    const TOffset end = offsets[row + 1] - base;
    return std::string_view(col.Data + start, static_cast<size_t>(end - start));
}

std::string FormatValue(const TColumn& col, int32_t row, const TTypePtr& type) {
    if (IsMasked(col, row)) {
        return "NULL";
    }
    if (!type) {
        return "?";
    }

    if (auto t = TMaybeType<TIntegerType>(type)) {
        auto intType = t.Cast();
        const char* ptr = col.Data + row * (intType->BitWidth() / 8);
        if (intType->IsSigned()) {
            switch (intType->BitWidth()) {
                case 8:  return std::format("{}", static_cast<int64_t>(*reinterpret_cast<const int8_t*>(ptr)));
                case 16: return std::format("{}", *reinterpret_cast<const int16_t*>(ptr));
                case 32: return std::format("{}", *reinterpret_cast<const int32_t*>(ptr));
                case 64: return std::format("{}", *reinterpret_cast<const int64_t*>(ptr));
            }
        } else {
            switch (intType->BitWidth()) {
                case 8:  return std::format("{}", static_cast<uint64_t>(*reinterpret_cast<const uint8_t*>(ptr)));
                case 16: return std::format("{}", *reinterpret_cast<const uint16_t*>(ptr));
                case 32: return std::format("{}", *reinterpret_cast<const uint32_t*>(ptr));
                case 64: return std::format("{}", *reinterpret_cast<const uint64_t*>(ptr));
            }
        }
    } else if (TMaybeType<TFloatType>(type)) {
        return std::format("{}", reinterpret_cast<const double*>(col.Data)[row]);
    } else if (TMaybeType<TBoolType>(type)) {
        return IsBitSet(col, row) ? "true" : "false";
    } else if (TMaybeType<TStringType>(type)) {
        if (col.OffsetWidth == 4) {
            return std::string(StringViewValue<int32_t>(col, row));
        }
        return std::string(StringViewValue<int64_t>(col, row));
    } else if (TMaybeType<TSymbolType>(type)) {
        return std::string(1, col.Data[row]);
    }

    return "?";
}

bool IsNumeric(const TTypePtr& type) {
    return static_cast<bool>(TMaybeType<TIntegerType>(type)) ||
           static_cast<bool>(TMaybeType<TFloatType>(type));
}

std::string CsvEscape(const std::string& value, char sep) {
    bool needsQuoting = value.find(sep) != std::string::npos
        || value.find('"') != std::string::npos
        || value.find('\n') != std::string::npos;
    if (!needsQuoting) {
        return value;
    }
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') {
            result += "\"\"";
        } else {
            result += c;
        }
    }
    result += '"';
    return result;
}

template <typename TOffset>
void WriteCsvString(std::ostream& out, const TColumn& col, int32_t row, char sep) {
    const auto view = StringViewValue<TOffset>(col, row);
    bool needsQuoting = view.find(sep) != std::string_view::npos
        || view.find('"') != std::string_view::npos
        || view.find('\n') != std::string_view::npos;
    if (!needsQuoting) {
        out.write(view.data(), static_cast<std::streamsize>(view.size()));
        return;
    }
    out.put('"');
    for (char c : view) {
        if (c == '"') {
            out.write("\"\"", 2);
        } else {
            out.put(c);
        }
    }
    out.put('"');
}

template <typename TOffset>
void WriteCsvStringRaw(std::ostream& out, const TColumn& col, int32_t row) {
    const auto view = StringViewValue<TOffset>(col, row);
    out.write(view.data(), static_cast<std::streamsize>(view.size()));
}

} // namespace

// TConsoleSink

TConsoleSink::TConsoleSink(const TSchema& schema, std::ostream& out)
    : Schema_(schema)
    , Out_(out)
{
    Widths_.resize(schema.Columns.size());
    for (size_t i = 0; i < schema.Columns.size(); ++i) {
        Widths_[i] = schema.Columns[i].Name.size();
    }
}

void TConsoleSink::Write(const TRowSet& rowSet) {
    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (rowSet.Selection && !rowSet.Selection[row]) {
            continue;
        }
        std::vector<std::string> formatted(rowSet.ColumnCount);
        for (int32_t c = 0; c < rowSet.ColumnCount; ++c) {
            formatted[c] = FormatValue(rowSet.Columns[c], row, Schema_.Columns[c].Type);
            if (formatted[c].size() > Widths_[c]) {
                Widths_[c] = formatted[c].size();
            }
        }
        Rows_.push_back(std::move(formatted));
    }
}

void TConsoleSink::Flush() {
    size_t numCols = Schema_.Columns.size();

    for (size_t c = 0; c < numCols; ++c) {
        if (c > 0) {
            Out_ << " | ";
        }
        Out_ << std::left << std::setw(static_cast<int>(Widths_[c])) << Schema_.Columns[c].Name;
    }
    Out_ << "\n";

    for (size_t c = 0; c < numCols; ++c) {
        if (c > 0) {
            Out_ << "-+-";
        }
        Out_ << std::string(Widths_[c], '-');
    }
    Out_ << "\n";

    for (const auto& row : Rows_) {
        for (size_t c = 0; c < numCols; ++c) {
            if (c > 0) {
                Out_ << " | ";
            }
            if (IsNumeric(Schema_.Columns[c].Type)) {
                Out_ << std::right << std::setw(static_cast<int>(Widths_[c])) << row[c];
            } else {
                Out_ << std::left << std::setw(static_cast<int>(Widths_[c])) << row[c];
            }
        }
        Out_ << "\n";
    }
}

// TCsvSink

TCsvSink::TCsvSink(const TSchema& schema, std::ostream& out, char separator, bool noEscape)
    : Schema_(schema)
    , Out_(out)
    , Separator_(separator)
    , NoEscape_(noEscape)
{}

void TCsvSink::Write(const TRowSet& rowSet) {
    if (!HeaderPrinted_) {
        for (size_t c = 0; c < Schema_.Columns.size(); ++c) {
            if (c > 0) {
                Out_ << Separator_;
            }
            Out_ << Schema_.Columns[c].Name;
        }
        Out_ << "\n";
        HeaderPrinted_ = true;
    }

    for (int32_t row = 0; row < rowSet.RowCount; ++row) {
        if (rowSet.Selection && !rowSet.Selection[row]) {
            continue;
        }
        for (int32_t c = 0; c < rowSet.ColumnCount; ++c) {
            if (c > 0) {
                Out_ << Separator_;
            }
            const auto& col = rowSet.Columns[c];
            const auto& type = Schema_.Columns[c].Type;
            if (NoEscape_ && TMaybeType<TStringType>(type)) {
                if (col.OffsetWidth == 4) {
                    WriteCsvStringRaw<int32_t>(Out_, col, row);
                } else {
                    WriteCsvStringRaw<int64_t>(Out_, col, row);
                }
            } else if (TMaybeType<TStringType>(type)) {
                if (col.OffsetWidth == 4) {
                    WriteCsvString<int32_t>(Out_, col, row, Separator_);
                } else {
                    WriteCsvString<int64_t>(Out_, col, row, Separator_);
                }
            } else {
                Out_ << CsvEscape(FormatValue(col, row, type), Separator_);
            }
        }
        Out_ << "\n";
    }
}

void TNullSink::Write(const TRowSet& rowSet) {
    Rows_ += rowSet.RowCount;
}

} // namespace NQqb
