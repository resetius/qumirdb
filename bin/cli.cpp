#include <qdb/exec/planner.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <readline/history.h>
#include <readline/readline.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class ESyntax {
    Sexpr,
    Sql,
};

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

NQdb::TSchema SchemaFromType(
    const NQumir::NAst::TTypePtr& type,
    std::vector<std::string>& names,
    std::vector<NQdb::TColumnSchema>& columns)
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
    return NQdb::TSchema{std::span<const NQdb::TColumnSchema>(columns)};
}

struct TConfig {
    ESyntax Syntax = ESyntax::Sexpr;
    std::string DataDir = ".";
    TFormatSpec Format;
    int MaxRowSets = -1;
    bool Verbose = false;
};

std::string ResolveTablePath(const std::string& dataDir, std::string_view table) {
    if (table.ends_with(".parquet")) {
        return std::string(table);
    }
    return dataDir + "/" + std::string(table) + ".parquet";
}

std::expected<NQdb::TOperatorPtr, NQumir::TError> ParseSexpr(
    std::istream& in,
    const std::function<NQdb::TOperatorPtr(const std::string&)>& makeSource)
{
    NQdb::NSexp::TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) {
        return makeSource(std::string(path));
    };

    NQumir::NAst::NCore::TParser parser;
    for (auto& [key, value] : NQdb::NSexp::MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[key] = std::move(value);
    }

    NQumir::NAst::NCore::TTokenStream tokens(in);
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto plan = std::dynamic_pointer_cast<NQdb::IOperator>(*parsed);
    if (!plan) {
        return std::unexpected(NQumir::TError("input must be a relational plan (rel ...)"));
    }
    return plan;
}

std::expected<NQdb::TOperatorPtr, NQumir::TError> ParseSql(
    std::istream& in,
    const NQdb::TTableSourceFactory& sources)
{
    NQdb::NSql::TTokenStream tokens(in);
    NQdb::NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return NQdb::BuildPlan(*parsed, sources);
}

int RunQuery(ESyntax syntax, std::istream& in, const TConfig& config) {
    std::vector<std::unique_ptr<NQdb::TParquetSource>> sources;
    auto makeSource = [&](const std::string& path) -> NQdb::TOperatorPtr {
        auto source = std::make_unique<NQdb::TParquetSource>(path);
        auto op = std::make_shared<NQdb::TSourceOperator>(*source, path);
        sources.push_back(std::move(source));
        return op;
    };

    std::expected<NQdb::TOperatorPtr, NQumir::TError> plan;
    if (syntax == ESyntax::Sql) {
        auto factory = [&](std::string_view table)
            -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
        {
            try {
                return makeSource(ResolveTablePath(config.DataDir, table));
            } catch (const std::exception& e) {
                return std::unexpected(NQumir::TError(
                    "cannot open table '" + std::string(table) + "': " + e.what()));
            }
        };
        plan = ParseSql(in, factory);
    } else {
        plan = ParseSexpr(in, makeSource);
    }

    if (!plan) {
        std::cerr << plan.error().ToString() << "\n";
        return 1;
    }

    NQdb::AssignSourceAliases(*plan);
    NQdb::QualifyColumns(*plan);
    NQdb::AnnotateTypes(*plan);
    NQdb::ApplyColumnPruning(*plan);

    if (config.Verbose) {
        std::cerr << "========== LOGICAL PLAN ==========\n";
        NQumir::NAst::NCore::PrintAst(
            std::cerr,
            *plan,
            NQumir::NAst::NCore::TPrintOptions{
                .NodePrinters = NQdb::NSexp::MakeRelPrinters(),
            });
        std::cerr << "\n==================================\n";
    }

    NQdb::TPhysicalPlanner planner(config.Verbose ? &std::cerr : nullptr);
    planner.PrintRuntimePlan(*plan);
    auto executor = planner.Build(*plan);

    std::vector<std::string> outputNames;
    std::vector<NQdb::TColumnSchema> outputColumns;
    auto schema = SchemaFromType(executor->OutputType(), outputNames, outputColumns);

    std::unique_ptr<NQdb::ISink> sink;
    if (config.Format.Name == "csv") {
        sink = std::make_unique<NQdb::TCsvSink>(
            schema, std::cout, config.Format.Separator, config.Format.NoEscape);
    } else if (config.Format.Name == "null") {
        sink = std::make_unique<NQdb::TNullSink>(schema);
    } else {
        sink = std::make_unique<NQdb::TConsoleSink>(schema, std::cout);
    }

    NQdb::TRowSet rowSet = {};
    int count = 0;
    auto start = std::chrono::steady_clock::now();
    while ((config.MaxRowSets < 0 || count < config.MaxRowSets) && executor->Next(rowSet)) {
        sink->Write(rowSet);
        NQdb::Release(&rowSet);
        ++count;
    }
    sink->Flush();
    auto elapsed = std::chrono::steady_clock::now() - start;
    std::cerr << "Processed " << count << " rowsets in "
              << std::chrono::duration<double>(elapsed).count() << " seconds\n";
    return 0;
}

// Reads SQL statements from readline and runs each as it is terminated by ';'.
int RunInteractive(const TConfig& config) {
    std::string buffer;
    while (true) {
        char* line = readline(buffer.empty() ? "qdb> " : "  ..> ");
        if (!line) {
            std::cout << "\n";
            break;
        }
        std::string input(line);
        std::free(line);

        if (buffer.empty()) {
            if (input == "exit" || input == "quit" || input == ".quit") {
                break;
            }
            if (input.empty()) {
                continue;
            }
        }

        buffer += input;
        buffer += '\n';
        if (buffer.find(';') == std::string::npos) {
            continue;
        }

        add_history(buffer.c_str());
        try {
            std::istringstream in(buffer);
            RunQuery(ESyntax::Sql, in, config);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
        }
        buffer.clear();
    }
    return 0;
}

void PrintHelp() {
    std::cout <<
        "qdb [options]\n"
        "Options:\n"
        "  -i <file>                    Input query file; without it, interactive mode\n"
        "  --sexpr                      Input file is an s-expression plan (default)\n"
        "  --sql                        Input file is SQL (built into a plan)\n"
        "  --data <dir>                 Directory of <table>.parquet files for SQL (default: .)\n"
        "  --format <spec>              Output format (default: console)\n"
        "    console                    Aligned table (PostgreSQL style)\n"
        "    csv                        CSV without escaping\n"
        "    <escape>csv                CSV with quoting/escaping\n"
        "    <separator=X>csv           CSV with custom separator X\n"
        "    null                       Consume rows without output\n"
        "  --rowsets <n>                Stop after n rowsets\n"
        "  --verbose                    Print the logical and runtime plans\n"
        "  --help|-h                    Show this help message\n"
        "\n"
        "Without -i, qdb starts an interactive SQL prompt (statements end with ';').\n";
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;

    TConfig config;
    std::string queryFile;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-i")) {
            if (i + 1 >= argc) {
                std::cerr << "-i requires a filename argument\n";
                return 1;
            }
            queryFile = argv[++i];
        } else if (!std::strcmp(argv[i], "--sexpr")) {
            config.Syntax = ESyntax::Sexpr;
        } else if (!std::strcmp(argv[i], "--sql")) {
            config.Syntax = ESyntax::Sql;
        } else if (!std::strcmp(argv[i], "--data")) {
            if (i + 1 >= argc) {
                std::cerr << "--data requires a directory argument\n";
                return 1;
            }
            config.DataDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--format")) {
            if (i + 1 >= argc) {
                std::cerr << "--format requires an argument\n";
                return 1;
            }
            try {
                config.Format = ParseFormat(argv[++i]);
            } catch (const std::invalid_argument& e) {
                std::cerr << "Invalid format spec: " << e.what() << "\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--rowsets")) {
            if (i + 1 >= argc) {
                std::cerr << "--rowsets requires an argument\n";
                return 1;
            }
            config.MaxRowSets = std::atoi(argv[++i]);
            if (config.MaxRowSets <= 0) {
                std::cerr << "--rowsets requires a positive integer\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--verbose")) {
            config.Verbose = true;
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            PrintHelp();
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    try {
        if (queryFile.empty()) {
            return RunInteractive(config);
        }

        std::ifstream ifs(queryFile);
        if (!ifs) {
            std::cerr << "Cannot open query file: " << queryFile << "\n";
            return 1;
        }
        return RunQuery(config.Syntax, ifs, config);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
