#pragma once

#include <qdb/io/io.h>
#include <qdb/io/schema.h>

#include <qumir/parser/type.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace NQdb {

// Disambiguates the column-schema constructor from the plain name list one.
struct TMockColumns {};

struct TMockSource : ISource {
    std::vector<std::string> Names; // owns backing strings (TColumnSchema::Name is a view)
    std::vector<TColumnSchema> Cols;
    TSchema Schema_;
    std::vector<TRowSet> Batches;
    size_t Index = 0;

    TMockSource() = default;

    // Column names (+ optional per-column types, default i64) and optional batches.
    explicit TMockSource(
        std::vector<std::string> names,
        std::vector<TRowSet> batches = {},
        std::vector<NQumir::NAst::TTypePtr> types = {})
        : Names(std::move(names))
        , Batches(std::move(batches))
    {
        BuildFromNames(types);
    }

    // Same, with the (types, batches) argument order some tests use.
    TMockSource(
        std::vector<std::string> names,
        std::vector<NQumir::NAst::TTypePtr> types,
        std::vector<TRowSet> batches)
        : Names(std::move(names))
        , Batches(std::move(batches))
    {
        BuildFromNames(types);
    }

    // Explicit column schema (+ optional batches); names are copied into Names so
    // the schema stays valid after the caller's columns go away.
    explicit TMockSource(
        TMockColumns,
        std::vector<TColumnSchema> columns,
        std::vector<TRowSet> batches = {})
        : Batches(std::move(batches))
    {
        Names.reserve(columns.size());
        for (const auto& column : columns) {
            Names.emplace_back(column.Name);
        }
        Cols.reserve(columns.size());
        for (size_t i = 0; i < columns.size(); ++i) {
            Cols.push_back({Names[i], columns[i].Type});
        }
        Schema_ = TSchema{Cols};
    }

    const TSchema& Schema() const override { return Schema_; }
    const TStatsPtr Stats() const override { return nullptr; }

    bool Next(TRowSet& rowSet) override {
        if (Index >= Batches.size()) {
            return false;
        }
        rowSet = Batches[Index++];
        return true;
    }

private:
    void BuildFromNames(const std::vector<NQumir::NAst::TTypePtr>& types) {
        Cols.reserve(Names.size());
        for (size_t i = 0; i < Names.size(); ++i) {
            Cols.push_back({
                Names[i],
                (i < types.size() && types[i])
                    ? types[i]
                    : std::make_shared<NQumir::NAst::TIntegerType>(
                          NQumir::NAst::TIntegerType::I64),
            });
        }
        Schema_ = TSchema{Cols};
    }
};

} // namespace NQdb
