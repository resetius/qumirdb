#include <qdb/exec/planner.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <readline/history.h>
#include <readline/readline.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <iostream>
#include <iterator>
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

NQdb::NScheduler::EExecutionMode ParseSchedulerMode(const std::string& mode) {
    if (mode == "serial") {
        return NQdb::NScheduler::EExecutionMode::Serial;
    }
    if (mode == "single") {
        return NQdb::NScheduler::EExecutionMode::SingleThreadedScheduler;
    }
    if (mode == "threaded") {
        return NQdb::NScheduler::EExecutionMode::ThreadedScheduler;
    }
    throw std::invalid_argument("unknown scheduler mode: " + mode);
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
    NQdb::NScheduler::TSettings Scheduler;
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

std::string ExprLine(const NQumir::NAst::TExprPtr& expr) {
    if (!expr) {
        return "";
    }
    return NQumir::NAst::NCore::PrintAst(expr, NQumir::NAst::NCore::TPrintOptions{ .Pretty = false });
}

std::string PlanLabel(const NQdb::TOperatorPtr& op) {
    using namespace NQdb;

    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        auto src = source.Cast();
        std::string label = "source " + src->SourcePath();
        if (!src->GetAlias().empty()) {
            label += " AS " + src->GetAlias();
        }
        return label;
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        return "filter " + ExprLine(filter.Cast()->Predicate());
    }
    if (auto project = TMaybeOp<TProjectOperator>(op)) {
        std::string label = "project (";
        const auto& specs = project.Cast()->Projections();
        for (size_t i = 0; i < specs.size(); ++i) {
            label += (i ? ", " : "") + specs[i].Name;
        }
        return label + ")";
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        auto agg = aggregate.Cast();
        std::string label = "aggregate";
        const auto& keys = agg->GroupKeys();
        for (size_t i = 0; i < keys.size(); ++i) {
            label += (i ? ", " : " keys=[") + keys[i];
        }
        if (!keys.empty()) {
            label += "]";
        }
        const auto& aggs = agg->Aggs();
        for (size_t i = 0; i < aggs.size(); ++i) {
            label += (i ? ", " : " aggs=[") + aggs[i].Name + "=" + aggs[i].Func;
        }
        if (!aggs.empty()) {
            label += "]";
        }
        return label;
    }
    if (auto join = TMaybeOp<TJoinOperator>(op)) {
        auto j = join.Cast();
        std::string label = "join " + std::string(JoinTypeName(j->JoinType()));
        const auto& keys = j->Keys();
        for (size_t i = 0; i < keys.size(); ++i) {
            label += (i ? ", " : " [") + keys[i].Left + " = " + keys[i].Right;
        }
        if (!keys.empty()) {
            label += "]";
        }
        if (j->Filter()) {
            label += " residual " + ExprLine(j->Filter());
        }
        return label;
    }
    return std::string(op->RelName());
}

std::vector<NQdb::TOperatorPtr> ChildOps(const NQdb::TOperatorPtr& op) {
    std::vector<NQdb::TOperatorPtr> out;
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<NQdb::IOperator>(child)) {
            out.push_back(childOp.Cast());
        }
    }
    return out;
}

// Box-drawing tree of the plan.
void PrintPlanTree(
    std::ostream& out,
    const NQdb::TOperatorPtr& op,
    const std::string& prefix = "",
    bool isLast = true,
    bool isRoot = true)
{
    out << prefix;
    if (!isRoot) {
        out << (isLast ? "└─ " : "├─ ");
    }
    out << PlanLabel(op) << "\n";

    auto children = ChildOps(op);
    std::string childPrefix = isRoot ? prefix : prefix + (isLast ? "   " : "│  ");
    for (size_t i = 0; i < children.size(); ++i) {
        PrintPlanTree(out, children[i], childPrefix, i + 1 == children.size(), false);
    }
}

// If `text` begins with the word "explain" (case-insensitive), strip it and
// return true.
bool StripExplain(std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return false;
    }
    static const std::string keyword = "explain";
    if (text.size() - start < keyword.size()) {
        return false;
    }
    for (size_t i = 0; i < keyword.size(); ++i) {
        if (std::tolower(text[start + i]) != keyword[i]) {
            return false;
        }
    }
    size_t after = start + keyword.size();
    if (after < text.size() && !std::isspace(text[after])) {
        return false; // e.g. "explained"
    }
    text = text.substr(after);
    return true;
}

int RunQuery(ESyntax syntax, std::istream& in, const TConfig& config) {
    std::vector<std::unique_ptr<NQdb::TParquetSource>> sources;
    auto makeSource = [&](const std::string& path) -> NQdb::TOperatorPtr {
        auto source = std::make_unique<NQdb::TParquetSource>(path);
        auto op = std::make_shared<NQdb::TSourceOperator>(*source, path);
        sources.push_back(std::move(source));
        return op;
    };

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bool explain = StripExplain(text);
    std::istringstream stream(text);

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
        plan = ParseSql(stream, factory);
    } else {
        plan = ParseSexpr(stream, makeSource);
    }

    if (!plan) {
        std::cerr << plan.error().ToString() << "\n";
        return 1;
    }

    NQdb::AssignSourceAliases(*plan);
    NQdb::QualifyColumns(*plan);
    NQdb::AnnotateTypes(*plan);
    *plan = NQdb::ReorderJoins(*plan);
    NQdb::AnnotateTypes(*plan); // re-annotate: reordering rebuilt the join tree
    *plan = NQdb::ExtractEquiJoins(*plan);
    NQdb::AnnotateTypes(*plan); // re-annotate: equi-join extraction adds/removes nodes
    *plan = NQdb::ApplyTopSort(*plan);
    NQdb::AnnotateTypes(*plan); // re-annotate: top-sort replaces limit(sort(...))
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

    if (explain) {
        NQumir::NAst::NCore::PrintAst(
            std::cout,
            *plan,
            NQumir::NAst::NCore::TPrintOptions{
                .NodePrinters = NQdb::NSexp::MakeRelPrinters(),
            });
        std::cout << "\n\n";
        PrintPlanTree(std::cout, *plan);
        return 0;
    }

    NQdb::TPhysicalPlanner planner(
        config.Verbose ? &std::cerr : nullptr,
        config.Scheduler);
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

std::string HistoryPath() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.qdb_history" : std::string();
}

// Multi-line statements are kept multi-line in the in-memory history (for proper
// recall), but persisted one escaped line per entry: '\n' -> "\\n", '\' -> "\\\\".
std::string EscapeHistory(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string UnescapeHistory(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[++i];
            out += (next == 'n') ? '\n' : next;
        } else {
            out += s[i];
        }
    }
    return out;
}

void LoadHistory(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            add_history(UnescapeHistory(line).c_str());
        }
    }
}

void SaveHistory(const std::string& path) {
    std::ofstream out(path);
    if (HIST_ENTRY** list = history_list()) {
        for (size_t i = 0; list[i]; ++i) {
            out << EscapeHistory(list[i]->line) << '\n';
        }
    }
}

std::string_view Trim(std::string_view s) {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == ';'; };
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// `\d <table>`: open the table and print its column names and types.
void DescribeTable(std::string_view table, const TConfig& config) {
    if (table.empty()) {
        std::cerr << "usage: \\d <table>\n";
        return;
    }
    const std::string path = ResolveTablePath(config.DataDir, table);
    try {
        NQdb::TParquetSource source(path);
        const auto& schema = source.Schema();
        std::cout << "Table \"" << table << "\" (" << path << ")\n";
        for (const auto& column : schema.Columns) {
            std::cout << "  " << column.Name << "  "
                      << NQumir::NAst::NCore::PrintType(column.Type) << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
    }
}

// Reads SQL statements from readline and runs each as it is terminated by ';'.
int RunInteractive(const TConfig& config) {
    const std::string historyPath = HistoryPath();
    if (!historyPath.empty()) {
        LoadHistory(historyPath);
    }

    // Strip trailing newlines but keep the statement multi-line for recall.
    auto trim = [](std::string buffer) {
        while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r')) {
            buffer.pop_back();
        }
        return buffer;
    };

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
            if (input.starts_with("\\d")) {
                DescribeTable(Trim(std::string_view(input).substr(2)), config);
                continue;
            }
        }

        buffer += input;
        buffer += '\n';
        if (buffer.find(';') == std::string::npos) {
            continue;
        }

        add_history(trim(buffer).c_str());
        if (!historyPath.empty()) {
            SaveHistory(historyPath);
        }
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
        "  --scheduler <mode>           Execution mode: serial, single, threaded\n"
        "  --scheduler-workers <n>      Worker count for threaded scheduler\n"
        "  --scan-tasks <n>             Maximum parquet scan tasks for scheduler mode\n"
        "  --scheduler-counters         Print scheduler and connection counters with --verbose\n"
        "  --cascade-aggregates         Parallelize global aggregates (partial->gather->combine)\n"
        "  --shuffle-partitions <n>     Hash shuffle partition count\n"
        "  --shuffle-queue <n>          Rowset queue capacity per shuffle lane\n"
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
        } else if (!std::strcmp(argv[i], "--scheduler")) {
            if (i + 1 >= argc) {
                std::cerr << "--scheduler requires an argument\n";
                return 1;
            }
            try {
                config.Scheduler.Scheduler.Mode = ParseSchedulerMode(argv[++i]);
            } catch (const std::invalid_argument& e) {
                std::cerr << e.what() << "\n";
                return 1;
            }
        } else if (!std::strcmp(argv[i], "--scheduler-workers")) {
            if (i + 1 >= argc) {
                std::cerr << "--scheduler-workers requires an argument\n";
                return 1;
            }
            auto workers = std::atoi(argv[++i]);
            if (workers <= 0) {
                std::cerr << "--scheduler-workers requires a positive integer\n";
                return 1;
            }
            config.Scheduler.Scheduler.WorkerCount = static_cast<size_t>(workers);
        } else if (!std::strcmp(argv[i], "--scan-tasks")) {
            if (i + 1 >= argc) {
                std::cerr << "--scan-tasks requires an argument\n";
                return 1;
            }
            auto tasks = std::atoi(argv[++i]);
            if (tasks <= 0) {
                std::cerr << "--scan-tasks requires a positive integer\n";
                return 1;
            }
            config.Scheduler.ScanSplit.MaxScanTasks = static_cast<size_t>(tasks);
        } else if (!std::strcmp(argv[i], "--scheduler-counters")) {
            config.Scheduler.Queue.EnableDebugCounters = true;
        } else if (!std::strcmp(argv[i], "--cascade-aggregates")) {
            config.Scheduler.Aggregate.CascadeGlobal = true;
        } else if (!std::strcmp(argv[i], "--shuffle-partitions")) {
            if (i + 1 >= argc) {
                std::cerr << "--shuffle-partitions requires an argument\n";
                return 1;
            }
            auto partitions = std::atoi(argv[++i]);
            if (partitions <= 0) {
                std::cerr << "--shuffle-partitions requires a positive integer\n";
                return 1;
            }
            config.Scheduler.HashShuffle.PartitionCount =
                static_cast<size_t>(partitions);
            config.Scheduler.HashShuffle.MaxPartitionCount =
                static_cast<size_t>(partitions);
        } else if (!std::strcmp(argv[i], "--shuffle-queue")) {
            if (i + 1 >= argc) {
                std::cerr << "--shuffle-queue requires an argument\n";
                return 1;
            }
            auto capacity = std::atoi(argv[++i]);
            if (capacity <= 0) {
                std::cerr << "--shuffle-queue requires a positive integer\n";
                return 1;
            }
            config.Scheduler.HashShuffle.MaxQueuedRowsetsPerLane =
                static_cast<size_t>(capacity);
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
