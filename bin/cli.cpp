#include <qdb/exec/planner.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/ops/source.h>
#include <qdb/sexp/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream> // used in ParseFormat
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TFormatSpec {
    std::string Name = "console";
    char Separator = ',';
    bool NoEscape = false;
};

// Format spec grammar:
//   csv                       CSV, no escaping
//   console                   aligned table
//   null                      no output
//   <attrs>format             format with attributes
//
// Attributes (comma-separated inside <>):
//   separator=X               use X as CSV separator
//   escape                    enable CSV quoting/escaping
//
// Examples: <escape>csv   <separator=|>csv   <separator=|,escape>csv
TFormatSpec ParseFormat(const std::string& spec) {
    TFormatSpec result;
    bool explicitEscape = false;

    if (spec.starts_with('<')) {
        auto close = spec.find('>');
        if (close == std::string::npos) {
            throw std::invalid_argument("unclosed '<' in format spec");
        }
        std::string attrs = spec.substr(1, close - 1);
        result.Name = spec.substr(close + 1);

        std::istringstream ss(attrs);
        std::string attr;
        while (std::getline(ss, attr, ',')) {
            if (attr == "escape") {
                result.NoEscape = false;
                explicitEscape = true;
            } else if (auto eq = attr.find('='); eq != std::string::npos) {
                std::string key = attr.substr(0, eq);
                std::string val = attr.substr(eq + 1);
                if (key == "separator") {
                    if (val.size() != 1) {
                        throw std::invalid_argument("separator must be a single character");
                    }
                    result.Separator = val[0];
                } else {
                    throw std::invalid_argument("unknown format attribute: " + key);
                }
            } else {
                throw std::invalid_argument("unknown format attribute: " + attr);
            }
        }
    } else {
        result.Name = spec;
    }

    if (result.Name != "console" && result.Name != "csv" && result.Name != "null") {
        throw std::invalid_argument("unknown format: " + result.Name);
    }
    if (result.Name == "csv" && !explicitEscape) {
        result.NoEscape = true;
    }
    return result;
}

NQqb::TSchema SchemaFromType(
    const NQumir::NAst::TTypePtr& type,
    std::vector<std::string>& names,
    std::vector<NQqb::TColumnSchema>& columns)
{
    auto* structType = static_cast<NQumir::NAst::TStructType*>(type.get());
    if (!structType) {
        throw std::runtime_error("executor output type must be TStructType");
    }
    names.clear();
    columns.clear();
    names.reserve(structType->Fields.size());
    columns.reserve(structType->Fields.size());
    for (const auto& [name, fieldType] : structType->Fields) {
        names.push_back(name);
    }
    for (size_t i = 0; i < structType->Fields.size(); ++i) {
        columns.push_back({.Name = names[i], .Type = structType->Fields[i].second});
    }
    return NQqb::TSchema{std::span<const NQqb::TColumnSchema>(columns)};
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;

    std::string queryFile;
    TFormatSpec formatSpec;
    int maxRowSets = -1;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-i")) {
            if (i + 1 < argc) {
                queryFile = argv[++i];
            } else {
                std::cerr << "-i requires a filename argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--format")) {
            if (i + 1 < argc) {
                try {
                    formatSpec = ParseFormat(argv[++i]);
                } catch (const std::invalid_argument& e) {
                    std::cerr << "Invalid format spec: " << e.what() << "\n";
                    return 1;
                }
            } else {
                std::cerr << "--format requires an argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--rowsets")) {
            if (i + 1 < argc) {
                maxRowSets = std::atoi(argv[++i]);
                if (maxRowSets <= 0) {
                    std::cerr << "--rowsets requires a positive integer\n";
                    return 1;
                }
            } else {
                std::cerr << "--rowsets requires an argument\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::cout <<
                "qdb [options]\n"
                "Options:\n"
                "  -i <file.sexp>               Query plan file (s-expression format)\n"
                "  --format <spec>              Output format (default: console)\n"
                "    console                    Aligned table (PostgreSQL style)\n"
                "    csv                        CSV without escaping\n"
                "    <escape>csv                CSV with quoting/escaping\n"
                "    <separator=X>csv           CSV with custom separator X\n"
                "    <separator=X,escape>csv    CSV with custom separator + escaping\n"
                "    null                       Consume rows without output\n"
                "  --rowsets <n>                Stop after n rowsets\n"
                "  --help|-h                    Show this help message\n"
                "\n"
                "Query file format (s-expression):\n"
                "  (rel source \"path/to/file.parquet\")\n"
                "  (rel filter <input> <predicate>)\n"
                "  (rel project <input> (out_col expr) ...)\n"
                "\n"
                "Example:\n"
                "  (rel project\n"
                "    (rel filter\n"
                "      (rel source \"orders.parquet\")\n"
                "      (> o_totalprice (: 400000.0 f64)))\n"
                "    (o_orderkey o_orderkey) (o_totalprice o_totalprice))\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    if (queryFile.empty()) {
        std::cerr << "No query file specified. Use -i <file.sexp>\n";
        return 1;
    }

    try {
        std::ifstream ifs(queryFile);
        if (!ifs) {
            std::cerr << "Cannot open query file: " << queryFile << "\n";
            return 1;
        }

        // Sources must outlive the plan
        std::vector<std::unique_ptr<NQqb::TParquetSource>> sources;

        NQqb::NSexp::TRelParserOptions parserOpts;
        parserOpts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> NQqb::TOperatorPtr {
            auto src = std::make_unique<NQqb::TParquetSource>(std::string(path));
            auto op = std::make_shared<NQqb::TSourceOperator>(*src, std::string(path));
            sources.push_back(std::move(src));
            return op;
        };

        NQumir::NAst::NCore::TParser parser;
        for (auto& [k, v] : NQqb::NSexp::MakeRelParsers(std::move(parserOpts))) {
            parser.NodeParsers[k] = std::move(v);
        }

        NQumir::NAst::NCore::TTokenStream tokens(ifs);
        auto parsed = parser.Parse(tokens);
        if (!parsed) {
            std::cerr << "Parse error: " << parsed.error().ToString() << "\n";
            return 1;
        }

        auto plan = std::dynamic_pointer_cast<NQqb::IOperator>(*parsed);
        if (!plan) {
            std::cerr << "Query file must contain a relational plan (rel ...)\n";
            return 1;
        }

        NQqb::ApplyColumnPruning(plan);

        NQqb::TPhysicalPlanner planner;
        auto executor = planner.Build(plan);

        std::vector<std::string> outputNames;
        std::vector<NQqb::TColumnSchema> outputColumns;
        auto schema = SchemaFromType(executor->OutputType(), outputNames, outputColumns);

        std::unique_ptr<NQqb::ISink> sink;
        if (formatSpec.Name == "csv") {
            sink = std::make_unique<NQqb::TCsvSink>(schema, std::cout, formatSpec.Separator, formatSpec.NoEscape);
        } else if (formatSpec.Name == "null") {
            sink = std::make_unique<NQqb::TNullSink>(schema);
        } else {
            sink = std::make_unique<NQqb::TConsoleSink>(schema, std::cout);
        }

        NQqb::TRowSet rowSet = {};
        int count = 0;
        auto start = std::chrono::steady_clock::now();
        while ((maxRowSets < 0 || count < maxRowSets) && executor->Next(rowSet)) {
            sink->Write(rowSet);
            NQqb::Release(&rowSet);
            ++count;
        }
        sink->Flush();
        auto elapsed = std::chrono::steady_clock::now() - start;
        std::cerr << "Processed " << count << " rowsets in "
                  << std::chrono::duration<double>(elapsed).count() << " seconds\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
