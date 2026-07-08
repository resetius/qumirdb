#include <qdb/exec/planner_helpers.h>
#include <qdb/io/io.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/top_sort.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/passes/unbound_vars.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/project_type.h>
#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/scheduler/scan_split.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/pragma.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>

#include <llvm/Support/Base64.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int64_t WasmPageSize = 65536;
constexpr int64_t WasmSlot0 = 1 << 20;
constexpr int64_t WasmSlotSize = 1 << 20;
constexpr int64_t WasmStackSize = 262144;
constexpr int64_t WasmInitialSlackPages = 256;

struct TTableStats {
    int64_t Rows = 0;
    int64_t Bytes = 0;
    size_t RowGroups = 0;
};

class TSchemaOnlySource final
    : public NQdb::ISource
    , public NQdb::NScheduler::IScanMetadataSource
{
public:
    explicit TSchemaOnlySource(
        std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields,
        TTableStats stats)
        : Stats_(stats)
    {
        Names_.reserve(fields.size());
        Columns_.reserve(fields.size());
        AllColumns_.reserve(fields.size());
        for (auto& [name, type] : fields) {
            Names_.push_back(std::move(name));
            AllColumns_.push_back({
                .Name = Names_.back(),
                .Type = std::move(type),
            });
        }
        Columns_ = AllColumns_;
        Schema_ = NQdb::TSchema{std::span<const NQdb::TColumnSchema>(Columns_)};
    }

    const NQdb::TSchema& Schema() const override {
        return Schema_;
    }

    bool Next(NQdb::TRowSet&) override {
        return false;
    }

    void RestrictColumns(const std::unordered_set<std::string>& names) override {
        if (names.empty()) {
            return;
        }

        Columns_.clear();
        for (const auto& column : AllColumns_) {
            if (names.contains(std::string(column.Name))) {
                Columns_.push_back(column);
            }
        }
        Schema_ = NQdb::TSchema{std::span<const NQdb::TColumnSchema>(Columns_)};
    }

    std::vector<NQdb::NScheduler::TScanRowGroup> ScanRowGroups() const override {
        if (Stats_.Rows <= 0 || Stats_.RowGroups == 0) {
            return {};
        }

        std::vector<NQdb::NScheduler::TScanRowGroup> out;
        out.reserve(Stats_.RowGroups);
        const auto groups = static_cast<int64_t>(Stats_.RowGroups);
        for (size_t i = 0; i < Stats_.RowGroups; ++i) {
            const auto begin =
                Stats_.Rows * static_cast<int64_t>(i) / groups;
            const auto end =
                Stats_.Rows * static_cast<int64_t>(i + 1) / groups;
            const auto byteBegin =
                Stats_.Bytes * static_cast<int64_t>(i) / groups;
            const auto byteEnd =
                Stats_.Bytes * static_cast<int64_t>(i + 1) / groups;
            out.push_back({
                .RowGroup = i,
                .RowCount = end - begin,
                .ByteSize = byteEnd - byteBegin,
            });
        }
        return out;
    }

private:
    TTableStats Stats_;
    std::vector<std::string> Names_;
    std::vector<NQdb::TColumnSchema> AllColumns_;
    std::vector<NQdb::TColumnSchema> Columns_;
    NQdb::TSchema Schema_;
};

struct TCatalog {
    std::unordered_map<std::string, std::unique_ptr<TSchemaOnlySource>> Sources;
};

struct TExportRequest {
    std::string Sql;
    TCatalog Catalog;
    NQdb::NScheduler::TSettings Scheduler;
    bool EmbedWasm = false;
};

struct TKernelArtifacts {
    std::string Stage;
    std::string Name;
    std::string Ast;
    std::string Ir;
    std::string Llvm;
    std::string Wasm;
};

class TArtifactStore {
public:
    std::string Add(
        std::string kind,
        std::string text,
        std::string label = {},
        std::string stage = {})
    {
        auto id = "a" + std::to_string(NextId_++);
        llvm::json::Object item{
            {"kind", std::move(kind)},
            {"encoding", "utf-8"},
            {"text", std::move(text)},
        };
        if (!label.empty()) {
            item["label"] = std::move(label);
        }
        if (!stage.empty()) {
            item["stage"] = std::move(stage);
        }
        Items_[id] = std::move(item);
        return id;
    }

    std::string AddBinary(
        std::string kind,
        std::string bytes,
        std::string label = {},
        std::string stage = {})
    {
        auto id = "a" + std::to_string(NextId_++);
        llvm::json::Object item{
            {"kind", std::move(kind)},
            {"encoding", "base64"},
            {"data", llvm::encodeBase64(bytes)},
            {"byteLength", static_cast<int64_t>(bytes.size())},
        };
        if (!label.empty()) {
            item["label"] = std::move(label);
        }
        if (!stage.empty()) {
            item["stage"] = std::move(stage);
        }
        Items_[id] = std::move(item);
        return id;
    }

    llvm::json::Object Take() {
        return std::move(Items_);
    }

private:
    size_t NextId_ = 0;
    llvm::json::Object Items_;
};

class TOutputSilencer {
public:
    TOutputSilencer() {
        std::fflush(stdout);
        std::fflush(stderr);
        SavedStdoutFd_ = dup(STDOUT_FILENO);
        SavedStderrFd_ = dup(STDERR_FILENO);
        int nullFd = open("/dev/null", O_WRONLY);
        if (SavedStdoutFd_ >= 0 && nullFd >= 0) {
            dup2(nullFd, STDOUT_FILENO);
        }
        if (SavedStderrFd_ >= 0 && nullFd >= 0) {
            dup2(nullFd, STDERR_FILENO);
        }
        if (nullFd >= 0) {
            close(nullFd);
        }
    }

    ~TOutputSilencer() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (SavedStdoutFd_ >= 0) {
            dup2(SavedStdoutFd_, STDOUT_FILENO);
            close(SavedStdoutFd_);
        }
        if (SavedStderrFd_ >= 0) {
            dup2(SavedStderrFd_, STDERR_FILENO);
            close(SavedStderrFd_);
        }
    }

private:
    int SavedStdoutFd_ = -1;
    int SavedStderrFd_ = -1;
};

std::string ReadAllStdin() {
    std::ostringstream out;
    out << std::cin.rdbuf();
    return out.str();
}

std::string ToJsonString(llvm::json::Value value) {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << value;
    return out;
}

llvm::json::Object ErrorObject(std::string stage, std::string message) {
    return llvm::json::Object{
        {"ok", false},
        {"error", llvm::json::Object{
            {"stage", std::move(stage)},
            {"message", std::move(message)},
        }},
    };
}

int PrintError(std::string stage, std::string message) {
    std::cout << ToJsonString(ErrorObject(std::move(stage), std::move(message)))
              << "\n";
    return 0;
}

std::filesystem::path TempPath(std::string_view suffix) {
    static uint64_t nextId = 0;
    auto name = "qdb-plan-export-" + std::to_string(::getpid()) + "-" +
        std::to_string(nextId++) + std::string(suffix);
    return std::filesystem::temp_directory_path() / name;
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

std::expected<std::monostate, std::string> RunWasmLd(
    const std::filesystem::path& objPath,
    const std::filesystem::path& wasmPath,
    int64_t globalBase)
{
    std::vector<std::string> args{
        "wasm-ld",
        "--no-entry",
        "--export-all",
        "--allow-undefined",
        "--import-memory",
        "-mwasm64",
        "--global-base=" + std::to_string(globalBase),
        "-z",
        "stack-size=" + std::to_string(WasmStackSize),
        "-o",
        wasmPath.string(),
        objPath.string(),
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        return std::unexpected("fork failed");
    }
    if (pid == 0) {
        int nullFd = open("/dev/null", O_WRONLY);
        if (nullFd >= 0) {
            dup2(nullFd, STDOUT_FILENO);
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return std::unexpected("waitpid failed");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(
            "wasm-ld failed with code " +
            std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
    }
    return std::monostate{};
}

// Compiles a kernel AST to WASM through the same path as the native JIT
// (NQdb::CompileKernelAstToObject), only swapping the codegen target. The target
// is wasm64 (8-byte pointers): the kernel/runtime layout assumes 8-byte pointers
// and wasm32 miscompiles it. See docs/issues/browser-wasm64-layout.md.
std::expected<std::string, std::string> CompileKernelAstToWasm(
    NQumir::NAst::TExprPtr ast,
    int64_t globalBase)
{
    auto opts = NQdb::KernelRunnerOptions();
    opts.NativeCode = false;
    opts.TargetTriple = "wasm64-unknown-unknown";
    NQumir::TLLVMRunner runner(std::move(opts));

    std::string err;
    auto object =
        NQdb::CompileKernelAstToObject(runner, std::move(ast), {}, &err);
    if (!object) {
        return std::unexpected(
            err.empty() ? "wasm kernel compilation failed" : err);
    }

    const auto objPath = TempPath(".o");
    const auto wasmPath = TempPath(".wasm");
    {
        std::ofstream obj(objPath, std::ios::binary);
        if (!obj) {
            return std::unexpected("cannot create temporary wasm object");
        }
        obj.write(object->data(), static_cast<std::streamsize>(object->size()));
    }

    auto cleanup = [&]() {
        std::error_code ec;
        std::filesystem::remove(objPath, ec);
        std::filesystem::remove(wasmPath, ec);
    };
    if (auto linked = RunWasmLd(objPath, wasmPath, globalBase); !linked) {
        cleanup();
        return std::unexpected(linked.error());
    }

    auto wasm = ReadBinaryFile(wasmPath);
    cleanup();
    if (wasm.empty()) {
        return std::unexpected("empty wasm output");
    }
    return wasm;
}

NQumir::NAst::TTypePtr ParseType(std::string_view typeName) {
    using namespace NQumir::NAst;
    using TInt = TIntegerType;

    if (typeName == "i8") {
        return std::make_shared<TInt>(TInt::I8);
    }
    if (typeName == "i16") {
        return std::make_shared<TInt>(TInt::I16);
    }
    if (typeName == "i32") {
        return std::make_shared<TInt>(TInt::I32);
    }
    if (typeName == "i64") {
        return std::make_shared<TInt>(TInt::I64);
    }
    if (typeName == "u8") {
        return std::make_shared<TInt>(TInt::U8);
    }
    if (typeName == "u16") {
        return std::make_shared<TInt>(TInt::U16);
    }
    if (typeName == "u32") {
        return std::make_shared<TInt>(TInt::U32);
    }
    if (typeName == "u64") {
        return std::make_shared<TInt>(TInt::U64);
    }
    if (typeName == "f64") {
        return std::make_shared<TFloatType>();
    }
    if (typeName == "bool") {
        return std::make_shared<TBoolType>();
    }
    if (typeName == "string") {
        return std::make_shared<TStringType>();
    }
    if (typeName == "date") {
        return std::make_shared<TInt>(TInt::I32);
    }

    throw std::runtime_error("unsupported schema type: " + std::string(typeName));
}

std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> ParseTableColumns(
    const llvm::json::Object& table)
{
    auto* columns = table.getArray("columns");
    if (!columns) {
        throw std::runtime_error("dataset table is missing columns array");
    }

    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> out;
    out.reserve(columns->size());
    for (const auto& columnValue : *columns) {
        const auto* column = columnValue.getAsObject();
        if (!column) {
            throw std::runtime_error("dataset column must be an object");
        }
        auto name = column->getString("name");
        auto type = column->getString("type");
        if (!name || !type) {
            throw std::runtime_error("dataset column must have name and type");
        }
        out.emplace_back(std::string(*name), ParseType(*type));
    }
    return out;
}

TTableStats ParseTableStats(const llvm::json::Object& table) {
    TTableStats stats;
    const auto* value = table.get("stats");
    const auto* object = value ? value->getAsObject() : nullptr;
    if (!object) {
        return stats;
    }

    if (auto rows = object->getInteger("rows")) {
        stats.Rows = std::max<int64_t>(*rows, 0);
    }
    if (auto bytes = object->getInteger("bytes")) {
        stats.Bytes = std::max<int64_t>(*bytes, 0);
    }
    if (auto rowGroups = object->getInteger("rowGroups")) {
        if (*rowGroups > 0) {
            stats.RowGroups = static_cast<size_t>(*rowGroups);
        }
    }
    if (stats.Bytes == 0 && stats.Rows > 0) {
        stats.Bytes = stats.Rows * 128;
    }
    if (stats.RowGroups == 0 && stats.Rows > 0) {
        stats.RowGroups = 1;
    }
    return stats;
}

TCatalog ParseCatalog(const llvm::json::Object& root) {
    const auto* datasetValue = root.get("dataset");
    const auto* dataset = datasetValue ? datasetValue->getAsObject() : nullptr;
    if (!dataset) {
        throw std::runtime_error("request is missing dataset object");
    }

    auto* tables = dataset->getArray("tables");
    if (!tables) {
        throw std::runtime_error("dataset is missing tables array");
    }

    TCatalog catalog;
    for (const auto& tableValue : *tables) {
        const auto* table = tableValue.getAsObject();
        if (!table) {
            throw std::runtime_error("dataset table must be an object");
        }
        auto name = table->getString("name");
        if (!name) {
            throw std::runtime_error("dataset table must have name");
        }
        catalog.Sources.emplace(
            std::string(*name),
            std::make_unique<TSchemaOnlySource>(
                ParseTableColumns(*table),
                ParseTableStats(*table)));
    }
    return catalog;
}

NQdb::NScheduler::EExecutionMode ParseSchedulerMode(std::string_view mode) {
    if (mode == "single") {
        return NQdb::NScheduler::EExecutionMode::SingleThreadedScheduler;
    }
    if (mode == "threaded") {
        return NQdb::NScheduler::EExecutionMode::ThreadedScheduler;
    }
    throw std::runtime_error("unknown scheduler mode: " + std::string(mode));
}

std::string SchedulerModeName(NQdb::NScheduler::EExecutionMode mode) {
    using NQdb::NScheduler::EExecutionMode;
    switch (mode) {
        case EExecutionMode::SingleThreadedScheduler:
            return "single";
        case EExecutionMode::ThreadedScheduler:
            return "threaded";
    }
    return "unknown";
}

NQdb::NScheduler::TSettings ParseSchedulerSettings(const llvm::json::Object& root) {
    NQdb::NScheduler::TSettings settings;

    const auto* optionsValue = root.get("options");
    const auto* options = optionsValue ? optionsValue->getAsObject() : nullptr;
    if (!options) {
        return settings;
    }

    if (auto scheduler = options->getString("scheduler")) {
        settings.Scheduler.Mode = ParseSchedulerMode(*scheduler);
    }
    if (auto workers = options->getInteger("schedulerWorkers")) {
        if (*workers > 0) {
            settings.Scheduler.WorkerCount = static_cast<size_t>(*workers);
        }
    }
    if (auto scanTasks = options->getInteger("scanTasks")) {
        if (*scanTasks > 0) {
            settings.ScanSplit.MaxScanTasks = static_cast<size_t>(*scanTasks);
        }
    }
    if (auto shuffleParts = options->getInteger("shufflePartitions")) {
        if (*shuffleParts > 0) {
            settings.HashShuffle.PartitionCount = static_cast<size_t>(*shuffleParts);
            settings.HashShuffle.MaxPartitionCount = static_cast<size_t>(*shuffleParts);
        }
    }
    return settings;
}

bool ParseEmbedWasm(const llvm::json::Object& root) {
    const auto* optionsValue = root.get("options");
    const auto* options = optionsValue ? optionsValue->getAsObject() : nullptr;
    if (!options) {
        return false;
    }
    return options->getBoolean("embedWasm").value_or(false);
}

std::expected<TExportRequest, std::string> ParseRequest(std::string_view text) {
    auto parsed = llvm::json::parse(text);
    if (!parsed) {
        return std::unexpected(llvm::toString(parsed.takeError()));
    }

    const auto* root = parsed->getAsObject();
    if (!root) {
        return std::unexpected("request root must be a JSON object");
    }

    auto sql = root->getString("sql");
    if (!sql || sql->trim().empty()) {
        return std::unexpected("missing sql");
    }

    try {
        return TExportRequest{
            .Sql = std::string(*sql),
            .Catalog = ParseCatalog(*root),
            .Scheduler = ParseSchedulerSettings(*root),
            .EmbedWasm = ParseEmbedWasm(*root),
        };
    } catch (const std::exception& e) {
        return std::unexpected(std::string(e.what()));
    }
}

std::expected<NQdb::TOperatorPtr, NQumir::TError> ParseSql(
    std::string_view sql,
    TCatalog& catalog)
{
    std::istringstream in{std::string(sql)};
    NQdb::NSql::TTokenStream tokens(in);
    NQdb::NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    NQdb::TTableSourceFactory sources = [&](std::string_view table)
        -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
    {
        auto it = catalog.Sources.find(std::string(table));
        if (it == catalog.Sources.end()) {
            return std::unexpected(NQumir::TError(
                "unknown browser dataset table: " + std::string(table)));
        }
        return std::make_shared<NQdb::TSourceOperator>(*it->second, std::string(table));
    };

    return NQdb::BuildPlan(*parsed, sources);
}

void ApplyPlanPasses(NQdb::TOperatorPtr& plan) {
    NQdb::AssignSourceAliases(plan);
    NQdb::QualifyColumns(plan);
    NQdb::AnnotateTypes(plan);
    plan = NQdb::ReorderJoins(plan);
    NQdb::AnnotateTypes(plan);
    plan = NQdb::ExtractEquiJoins(plan);
    NQdb::AnnotateTypes(plan);
    plan = NQdb::ApplyTopSort(plan);
    NQdb::AnnotateTypes(plan);
    NQdb::ApplyColumnPruning(plan);
}

std::string ExprLine(const NQumir::NAst::TExprPtr& expr) {
    if (!expr) {
        return "";
    }
    return NQumir::NAst::NCore::PrintAst(
        expr,
        NQumir::NAst::NCore::TPrintOptions{.Pretty = false});
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

void CollectPlanLabelsPostOrder(
    const NQdb::TOperatorPtr& op,
    std::vector<std::string>& labels)
{
    for (const auto& child : ChildOps(op)) {
        CollectPlanLabelsPostOrder(child, labels);
    }
    labels.push_back(PlanLabel(op));
}

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

std::string LogicalPlanTreeText(const NQdb::TOperatorPtr& plan) {
    std::ostringstream out;
    PrintPlanTree(out, plan);
    return out.str();
}

std::string LogicalPlanAstText(const NQdb::TOperatorPtr& plan) {
    std::ostringstream out;
    NQumir::NAst::NCore::PrintAst(
        out,
        plan,
        NQumir::NAst::NCore::TPrintOptions{
            .NodePrinters = NQdb::NSexp::MakeRelPrinters(),
        });
    return out.str();
}

std::string LogicalNodeAstText(const NQdb::TOperatorPtr& op) {
    std::ostringstream out;
    NQumir::NAst::NCore::PrintAst(
        out,
        op,
        NQumir::NAst::NCore::TPrintOptions{
            .NodePrinters = NQdb::NSexp::MakeRelPrinters(),
        });
    return out.str();
}

std::string Trim(std::string_view text) {
    while (!text.empty() &&
           (text.front() == '\n' || text.front() == '\r' ||
            text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' ||
            text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string ArtifactSlice(
    std::string_view text,
    std::string_view beginMarker,
    std::string_view endMarker)
{
    auto begin = text.find(beginMarker);
    if (begin == std::string_view::npos) {
        return {};
    }
    begin += beginMarker.size();
    auto end = text.find(endMarker, begin);
    if (end == std::string_view::npos) {
        end = text.size();
    }
    return Trim(text.substr(begin, end - begin));
}

void MaybeAddArtifactRef(
    llvm::json::Object& refs,
    std::string name,
    std::string id)
{
    if (id.empty()) {
        return;
    }
    refs[std::move(name)] = std::move(id);
}

void AddArrayArtifactRef(
    llvm::json::Object& refs,
    std::string name,
    std::string id)
{
    if (id.empty()) {
        return;
    }
    auto* value = refs.get(name);
    if (!value) {
        llvm::json::Array array;
        array.push_back(std::move(id));
        refs[std::move(name)] = std::move(array);
        return;
    }
    if (auto* array = value->getAsArray()) {
        array->push_back(std::move(id));
    }
}


std::string ConnectionKindName(NQdb::NScheduler::EConnectionKind kind) {
    using NQdb::NScheduler::EConnectionKind;
    switch (kind) {
        case EConnectionKind::OneToOne:
            return "one-to-one";
        case EConnectionKind::Gather:
            return "gather";
        case EConnectionKind::HashShuffle:
            return "hash-shuffle";
        case EConnectionKind::Broadcast:
            return "broadcast";
    }
    return "unknown";
}

std::string PlanLabelKind(std::string_view label) {
    auto space = label.find(' ');
    return std::string(label.substr(0, space));
}

llvm::json::Array StringArray(const std::vector<std::string>& values) {
    llvm::json::Array out;
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

llvm::json::Array SortKeysJson(const std::vector<NQdb::TSortKey>& keys) {
    llvm::json::Array out;
    for (const auto& key : keys) {
        out.push_back(llvm::json::Object{
            {"column", key.Column},
            {"direction", std::string(NQdb::SortDirectionName(key.Direction))},
            {"nulls", std::string(NQdb::SortNullsName(key.Nulls))},
        });
    }
    return out;
}

std::string LogicalNodeKind(const NQdb::TOperatorPtr& op) {
    return std::string(op->RelName());
}

llvm::json::Object LogicalNodeDetails(const NQdb::TOperatorPtr& op) {
    using namespace NQdb;

    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        auto src = source.Cast();
        return llvm::json::Object{
            {"path", src->SourcePath()},
            {"alias", src->GetAlias()},
        };
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        return llvm::json::Object{
            {"predicate", ExprLine(filter.Cast()->Predicate())},
        };
    }
    if (auto project = TMaybeOp<TProjectOperator>(op)) {
        llvm::json::Array projections;
        for (const auto& spec : project.Cast()->Projections()) {
            projections.push_back(llvm::json::Object{
                {"name", spec.Name},
                {"expr", ExprLine(spec.Expression)},
            });
        }
        return llvm::json::Object{
            {"projections", std::move(projections)},
        };
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        auto agg = aggregate.Cast();
        llvm::json::Array aggs;
        for (const auto& spec : agg->Aggs()) {
            aggs.push_back(llvm::json::Object{
                {"name", spec.Name},
                {"func", spec.Func},
                {"arg", ExprLine(spec.Arg)},
            });
        }
        return llvm::json::Object{
            {"groupKeys", StringArray(agg->GroupKeys())},
            {"aggregates", std::move(aggs)},
        };
    }
    if (auto join = TMaybeOp<TJoinOperator>(op)) {
        auto j = join.Cast();
        llvm::json::Array keys;
        for (const auto& key : j->Keys()) {
            keys.push_back(llvm::json::Object{
                {"left", key.Left},
                {"right", key.Right},
            });
        }
        return llvm::json::Object{
            {"joinType", std::string(JoinTypeName(j->JoinType()))},
            {"keys", std::move(keys)},
            {"residual", ExprLine(j->Filter())},
        };
    }
    if (auto sort = TMaybeOp<TSortOperator>(op)) {
        return llvm::json::Object{
            {"keys", SortKeysJson(sort.Cast()->Keys())},
        };
    }
    if (auto sort = TMaybeOp<TTopSortOperator>(op)) {
        return llvm::json::Object{
            {"keys", SortKeysJson(sort.Cast()->Keys())},
            {"limit", sort.Cast()->Limit()},
        };
    }
    if (auto limit = TMaybeOp<TLimitOperator>(op)) {
        return llvm::json::Object{
            {"limit", limit.Cast()->Limit()},
            {"offset", limit.Cast()->Offset()},
        };
    }
    return llvm::json::Object{};
}

llvm::json::Object GraphJson(
    const NQdb::NScheduler::TLoweredPlan& lowered,
    const std::vector<TKernelArtifacts>& kernelArtifacts)
{
    struct TGroup {
        std::string Id;
        std::string Kind;
        std::string Label;
        std::string Group;
        size_t TaskCount = 0;
        size_t InputCount = 0;
        size_t OutputCount = 0;
        std::vector<std::string> TaskIds;
    };
    struct TEdge {
        std::string From;
        std::string To;
        std::string Connection;
        size_t EdgeCount = 0;
    };

    std::vector<TGroup> groups;
    std::unordered_map<std::string, size_t> groupIndexes;
    std::unordered_map<const NQdb::NScheduler::TTaskNode*, std::string> nodeGroups;
    const auto& graphNodes = lowered.Graph->Nodes();
    nodeGroups.reserve(graphNodes.size());
    for (size_t i = 0; i < graphNodes.size(); ++i) {
        const auto& node = *graphNodes[i];
        const auto rawId = "n" + std::to_string(i);
        const auto kind = node.DebugKind.empty() ? "task" : node.DebugKind;
        const auto label = node.DebugLabel.empty() ? kind : node.DebugLabel;
        const auto groupKey = node.DebugGroup.empty()
            ? rawId
            : node.DebugGroup;

        auto it = groupIndexes.find(groupKey);
        if (it == groupIndexes.end()) {
            const auto index = groups.size();
            it = groupIndexes.emplace(groupKey, index).first;
            groups.push_back(TGroup{
                .Id = "g" + std::to_string(index),
                .Kind = kind,
                .Label = label,
                .Group = groupKey,
            });
        }

        auto& group = groups[it->second];
        ++group.TaskCount;
        group.InputCount += node.Inbound.size();
        group.OutputCount += node.Outbound.size();
        group.TaskIds.push_back(rawId);
        nodeGroups.emplace(graphNodes[i].get(), group.Id);
    }

    llvm::json::Array connections;
    std::unordered_map<const NQdb::NScheduler::IConnection*, std::string> connectionIds;
    auto addConnection = [&](const NQdb::NScheduler::IConnection* connection) {
        if (!connection || connectionIds.contains(connection)) {
            return;
        }
        auto id = "c" + std::to_string(connectionIds.size());
        connectionIds.emplace(connection, id);
        connections.push_back(llvm::json::Object{
            {"id", id},
            {"kind", ConnectionKindName(connection->Kind())},
            {"debugName", connection->DebugName()},
            {"srcCount", static_cast<int64_t>(connection->SrcCount())},
            {"dstCount", static_cast<int64_t>(connection->DstCount())},
        });
    };

    addConnection(lowered.FinalGather);
    for (const auto& edge : lowered.Graph->Edges()) {
        addConnection(edge->Connection);
    }

    llvm::json::Array edges;
    std::vector<TEdge> groupedEdges;
    std::unordered_map<std::string, size_t> edgeIndexes;
    for (const auto& edge : lowered.Graph->Edges()) {
        const auto& from = nodeGroups.at(edge->Src);
        const auto& to = nodeGroups.at(edge->Dst);
        const auto& connection = connectionIds.at(edge->Connection);
        const auto edgeKey = from + "\n" + to + "\n" + connection;
        auto it = edgeIndexes.find(edgeKey);
        if (it == edgeIndexes.end()) {
            const auto index = groupedEdges.size();
            it = edgeIndexes.emplace(edgeKey, index).first;
            groupedEdges.push_back(TEdge{
                .From = from,
                .To = to,
                .Connection = connection,
            });
        }
        ++groupedEdges[it->second].EdgeCount;
    }

    llvm::json::Array nodes;
    for (const auto& group : groups) {
        llvm::json::Array taskIds;
        for (const auto& taskId : group.TaskIds) {
            taskIds.push_back(taskId);
        }
        llvm::json::Object artifactRefs;
        for (const auto& item : kernelArtifacts) {
            if (item.Stage != group.Group) {
                continue;
            }
            AddArrayArtifactRef(artifactRefs, "ast", item.Ast);
            AddArrayArtifactRef(artifactRefs, "ir", item.Ir);
            AddArrayArtifactRef(artifactRefs, "llvm", item.Llvm);
            AddArrayArtifactRef(artifactRefs, "wasm", item.Wasm);
        }

        llvm::json::Object node{
            {"id", group.Id},
            {"kind", group.Kind},
            {"label", group.TaskCount > 1
                ? group.Label + " x" + std::to_string(group.TaskCount)
                : group.Label},
            {"taskCount", static_cast<int64_t>(group.TaskCount)},
            {"taskGroup", group.Group},
            {"inputCount", static_cast<int64_t>(group.InputCount)},
            {"outputCount", static_cast<int64_t>(group.OutputCount)},
            {"details", llvm::json::Object{
                {"kind", group.Kind},
                {"label", group.Label},
                {"taskCount", static_cast<int64_t>(group.TaskCount)},
                {"taskIds", std::move(taskIds)},
            }},
        };
        if (!artifactRefs.empty()) {
            node["artifacts"] = std::move(artifactRefs);
        }
        nodes.push_back(std::move(node));
    }

    for (const auto& edge : groupedEdges) {
        edges.push_back(llvm::json::Object{
            {"from", edge.From},
            {"to", edge.To},
            {"connection", edge.Connection},
            {"edgeCount", static_cast<int64_t>(edge.EdgeCount)},
        });
    }

    return llvm::json::Object{
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
        {"edges", std::move(edges)},
        {"result", llvm::json::Object{
            {"connection", connectionIds.at(lowered.FinalGather)},
            {"lanes", static_cast<int64_t>(lowered.Lanes)},
        }},
    };
}

llvm::json::Object EmptyGraphJson(std::string reason = {}) {
    llvm::json::Object result{
        {"nodes", llvm::json::Array{}},
        {"connections", llvm::json::Array{}},
        {"edges", llvm::json::Array{}},
    };
    if (!reason.empty()) {
        result["error"] = std::move(reason);
    }
    return result;
}

std::string AddLogicalGraphNode(
    const NQdb::TOperatorPtr& op,
    llvm::json::Array& nodes,
    llvm::json::Array& edges,
    TArtifactStore& artifacts,
    size_t& nextNodeId,
    size_t& nextConnectionId)
{
    std::vector<std::string> childIds;
    for (const auto& child : ChildOps(op)) {
        childIds.push_back(AddLogicalGraphNode(
            child,
            nodes,
            edges,
            artifacts,
            nextNodeId,
            nextConnectionId));
    }

    const std::string nodeId = "ln" + std::to_string(nextNodeId++);
    const auto label = LogicalNodeKind(op);
    const auto tooltip = PlanLabel(op);
    llvm::json::Object artifactRefs;
    MaybeAddArtifactRef(
        artifactRefs,
        "ast",
        artifacts.Add("ast", LogicalNodeAstText(op), tooltip));
    nodes.push_back(llvm::json::Object{
        {"id", nodeId},
        {"kind", label},
        {"label", label},
        {"tooltip", tooltip},
        {"artifacts", std::move(artifactRefs)},
        {"details", LogicalNodeDetails(op)},
        {"inputCount", static_cast<int64_t>(childIds.size())},
        {"outputCount", 1},
    });

    for (const auto& childId : childIds) {
        const auto connectionId = "lc" + std::to_string(nextConnectionId++);
        edges.push_back(llvm::json::Object{
            {"from", childId},
            {"to", nodeId},
            {"connection", connectionId},
            {"srcLane", 0},
            {"dstLane", 0},
        });
    }
    return nodeId;
}

llvm::json::Object LogicalGraphJson(
    const NQdb::TOperatorPtr& plan,
    TArtifactStore& artifacts)
{
    llvm::json::Array nodes;
    llvm::json::Array edges;
    size_t nextNodeId = 0;
    size_t nextConnectionId = 0;
    const auto rootId = AddLogicalGraphNode(
        plan,
        nodes,
        edges,
        artifacts,
        nextNodeId,
        nextConnectionId);

    llvm::json::Array connections;
    for (size_t i = 0; i < nextConnectionId; ++i) {
        connections.push_back(llvm::json::Object{
            {"id", "lc" + std::to_string(i)},
            {"kind", "logical"},
            {"debugName", "logical-flow"},
            {"srcCount", 1},
            {"dstCount", 1},
        });
    }

    return llvm::json::Object{
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
        {"edges", std::move(edges)},
        {"result", llvm::json::Object{
            {"node", rootId},
            {"lanes", 1},
        }},
    };
}

std::string AstText(const NQumir::NAst::TExprPtr& ast) {
    std::ostringstream out;
    NQumir::NAst::NCore::PrintAst(out, ast);
    return out.str();
}

// Register every lowered kernel's AST as an artifact and (in embed mode)
// compile it to wasm. Failures land in `diagnostics` per kernel; the bundle
// still forms.
std::vector<TKernelArtifacts> WasmFinalizeKernels(
    std::span<const NQdb::TGeneratedKernel> kernels,
    TArtifactStore& artifacts,
    llvm::json::Array& diagnostics,
    bool embedWasm)
{
    std::vector<TKernelArtifacts> out;
    out.reserve(kernels.size());
    for (size_t i = 0; i < kernels.size(); ++i) {
        const auto& kernel = kernels[i];
        TKernelArtifacts item{
            .Stage = kernel.Stage,
            .Name = kernel.Name,
        };
        item.Ast = artifacts.Add(
            "ast", AstText(kernel.Ast), item.Name, item.Stage);
        if (embedWasm) {
            // One generated kernel artifact owns one static slot. Reusing the
            // same artifact from multiple graph nodes is safe in the browser's
            // single-threaded scheduler: literals are immutable and wasm stack
            // use is not concurrent.
            const int64_t globalBase =
                WasmSlot0 + static_cast<int64_t>(i) * WasmSlotSize;
            auto wasm = CompileKernelAstToWasm(kernel.Ast, globalBase);
            if (wasm) {
                item.Wasm = artifacts.AddBinary(
                    "wasm", std::move(*wasm), item.Name, item.Stage);
            } else {
                diagnostics.push_back(llvm::json::Object{
                    {"stage", item.Stage},
                    {"message", wasm.error()},
                });
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}


std::string BareColumnName(std::string_view qualified) {
    auto dot = qualified.rfind('.');
    return std::string(
        dot == std::string_view::npos ? qualified : qualified.substr(dot + 1));
}

llvm::json::Object CoreTypeJson(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    const bool nullable = NQdb::IsNullableType(type);
    auto inner = UnwrapNamedType(NQdb::UnwrapNullableType(type));
    std::string name;
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        name = integer.Cast()->ToString();
    } else if (TMaybeType<TFloatType>(inner)) {
        name = "f64";
    } else if (TMaybeType<TBoolType>(inner)) {
        name = "bool";
    } else if (TMaybeType<TStringType>(inner)) {
        name = "string";
    } else {
        name = inner ? inner->ToString() : "unknown";
    }
    return llvm::json::Object{
        {"type", std::move(name)},
        {"nullable", nullable},
    };
}

bool IsStringCoreType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TStringType>(
        UnwrapNamedType(NQdb::UnwrapNullableType(type))));
}

// True if `expr` reads a string-typed column of `input`. Filter/project kernels
// that read a string column currently miscompile on the wasm -O3 backend (the
// StringView build is treated as UB and the whole kernel body is deleted), so
// such pipelines are reported unsupported rather than silently returning wrong
// rows. See PLAN_BROWSER_EXECUTION.md and the qumir string-codegen issue.
bool ReferencesStringColumn(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& input)
{
    auto refs = NQdb::FindUnboundVars(expr);
    for (const auto& [name, type] : input.Fields) {
        if (refs.contains(name) && IsStringCoreType(type)) {
            return true;
        }
    }
    return false;
}

size_t ExecProjectWidth(const NQumir::NAst::TTypePtr& outType) {
    using namespace NQumir::NAst;
    auto inner = UnwrapNamedType(NQdb::UnwrapNullableType(outType));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        return static_cast<size_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(inner)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(inner)) {
        return 1;
    }
    if (TMaybeType<TStringType>(inner)) {
        return 16; // sizeof(StringView)
    }
    return 0;
}

// Struct layout constants for the wasm32 target (verified against compiled
// kernels): qumir models pointers as 8 bytes on every target, so these offsets
// also match the native 64-bit layout. See PLAN_BROWSER_EXECUTION.md and
// docs/issues/qumir_pointer_width_and_pointer_cast.md.
llvm::json::Object ExecLayoutJson(size_t kernelCount) {
    const int64_t heapBase =
        WasmSlot0 + static_cast<int64_t>(kernelCount) * WasmSlotSize;
    const int64_t initialPages =
        (heapBase + WasmPageSize - 1) / WasmPageSize + WasmInitialSlackPages;
    llvm::json::Object layout{
        {"pointerSize", 8},
        {"column", llvm::json::Object{
            {"size", 48},
            {"data", 0},
            {"mask", 16},
            {"offsets", 32},
            {"offsetWidth", 40},
        }},
        {"rowset", llvm::json::Object{
            {"size", 56},
            {"columns", 0},
            {"columnCount", 8},
            {"rowCount", 16},
            {"selection", 24},
        }},
        {"stringView", llvm::json::Object{
            {"size", 16},
            {"data", 0},
            {"length", 8},
        }},
        // THashTable per modules/qumirdb.cpp; the runtime only reads Size.
        {"hashTable", llvm::json::Object{
            {"size", static_cast<int64_t>(NQdb::TKernelCompiler::kHashTableSize)},
            {"sizeOffset", 72},
        }},
        {"pairBuffer", llvm::json::Object{
            {"size", static_cast<int64_t>(NQdb::TKernelCompiler::kPairBufferSize)},
            {"count", 0},
            {"capacity", 8},
            {"data", 16},
        }},
    };
    layout["sharedMemory"] = llvm::json::Object{
        {"heapBase", heapBase},
        {"initialPages", initialPages},
        {"slot0", WasmSlot0},
        {"slotSize", WasmSlotSize},
        {"stackSize", WasmStackSize},
        {"kernelCount", static_cast<int64_t>(kernelCount)},
    };
    return layout;
}

llvm::json::Array ProjectOutputJson(
    NQdb::TProjectOperator& project,
    const NQumir::NAst::TStructType& inputStruct,
    const NQumir::NAst::TTypePtr& outputType)
{
    using namespace NQumir::NAst;
    auto* outStruct = static_cast<TStructType*>(outputType.get());
    llvm::json::Array out;
    size_t computedIndex = 0;
    const auto& projections = project.Projections();
    for (size_t i = 0; i < projections.size(); ++i) {
        const auto& projection = projections[i];
        const auto& outType = outStruct->Fields[i].second;
        auto entry = CoreTypeJson(outType);
        entry["name"] = projection.Name;
        if (auto identNode = TMaybeNode<TIdentExpr>(projection.Expression)) {
            const std::string& refName = identNode.Cast()->Name;
            int32_t inputIndex = -1;
            for (size_t f = 0; f < inputStruct.Fields.size(); ++f) {
                if (inputStruct.Fields[f].first == refName) {
                    inputIndex = static_cast<int32_t>(f);
                    break;
                }
            }
            entry["source"] = "passthrough";
            entry["inputIndex"] = inputIndex;
        } else {
            const bool isString = static_cast<bool>(TMaybeType<TStringType>(
                UnwrapNamedType(NQdb::UnwrapNullableType(outType))));
            entry["source"] = "computed";
            entry["computedIndex"] = static_cast<int64_t>(computedIndex++);
            entry["width"] = static_cast<int64_t>(ExecProjectWidth(outType));
            entry["isString"] = isString;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

llvm::json::Array SourceColumnsJson(const NQumir::NAst::TTypePtr& sourceType) {
    auto* st = static_cast<NQumir::NAst::TStructType*>(sourceType.get());
    llvm::json::Array out;
    for (const auto& [qname, type] : st->Fields) {
        auto entry = CoreTypeJson(type);
        entry["name"] = BareColumnName(qname);
        out.push_back(std::move(entry));
    }
    return out;
}

llvm::json::Array StructColumnsJson(const NQumir::NAst::TStructType& type) {
    llvm::json::Array out;
    for (const auto& [qname, fieldType] : type.Fields) {
        auto entry = CoreTypeJson(fieldType);
        entry["name"] = BareColumnName(qname);
        out.push_back(std::move(entry));
    }
    return out;
}

// Resolve sort keys to indices into the current batch's columns (matching by
// bare column name). Returns nullopt if any key column is not present.
std::optional<llvm::json::Array> ExecSortKeysJson(
    const std::vector<NQdb::TSortKey>& keys,
    const NQumir::NAst::TStructType& inputStruct)
{
    llvm::json::Array out;
    for (const auto& key : keys) {
        int32_t index = -1;
        for (size_t f = 0; f < inputStruct.Fields.size(); ++f) {
            if (BareColumnName(inputStruct.Fields[f].first) ==
                BareColumnName(key.Column))
            {
                index = static_cast<int32_t>(f);
                break;
            }
        }
        if (index < 0) {
            return std::nullopt;
        }
        out.push_back(llvm::json::Object{
            {"index", index},
            {"direction", std::string(NQdb::SortDirectionName(key.Direction))},
            {"nulls", std::string(NQdb::SortNullsName(key.Nulls))},
        });
    }
    return out;
}

llvm::json::Object UnsupportedExec(std::string reason) {
    return llvm::json::Object{
        {"supported", false},
        {"reason", std::move(reason)},
    };
}

bool BrowserSupportsJoin(NQdb::EJoinType type, bool hasResidual) {
    using NQdb::EJoinType;
    if (type == EJoinType::Inner) {
        return true;
    }
    if ((type == EJoinType::Left || type == EJoinType::Right) && !hasResidual) {
        return true;
    }
    // Semi/anti with residual compiles as an inner kernel; the runtime dedups
    // matched left ids and finalizes with a JS scan.
    if (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) {
        return true;
    }
    return false;
}

// One lowered kernel joined with its exported artifacts, indexed by the
// logical operator that generated it.
struct TKernelRef {
    const NQdb::TGeneratedKernel* Kernel = nullptr;
    const TKernelArtifacts* Artifacts = nullptr;
};
using TKernelIndex =
    std::unordered_map<const void*, std::vector<TKernelRef>>;

TKernelIndex BuildKernelIndex(
    std::span<const NQdb::TGeneratedKernel> kernels,
    std::span<const TKernelArtifacts> artifacts)
{
    TKernelIndex index;
    for (size_t i = 0; i < kernels.size(); ++i) {
        if (kernels[i].Operator) {
            index[kernels[i].Operator].push_back({&kernels[i], &artifacts[i]});
        }
    }
    return index;
}

const TKernelRef* FindKernel(
    const TKernelIndex& index, const void* op, std::string_view name)
{
    auto it = index.find(op);
    if (it == index.end()) {
        return nullptr;
    }
    for (const auto& ref : it->second) {
        if (ref.Kernel->Name == name) {
            return &ref;
        }
    }
    return nullptr;
}

// Builds a sort stage body from the operator's lowered kernels. When the sort
// compiled to a radix kernel with wasm, emits the kernel plus its per-key
// metadata; otherwise emits JS-comparison keys. Returns nullopt if a key
// references an unknown column.
std::optional<llvm::json::Object> BuildSortStageJson(
    const std::vector<NQdb::TSortKey>& sortKeys,
    const NQumir::NAst::TStructType& inputStruct,
    const TKernelIndex& kernels,
    const void* op,
    bool embedWasm)
{
    bool anyNullableKey = false;
    for (const auto& key : sortKeys) {
        int32_t index = -1;
        for (size_t f = 0; f < inputStruct.Fields.size(); ++f) {
            if (BareColumnName(inputStruct.Fields[f].first) == BareColumnName(key.Column)) {
                index = static_cast<int32_t>(f);
                break;
            }
        }
        if (index < 0) {
            return std::nullopt;
        }
        anyNullableKey = anyNullableKey ||
            NQdb::IsNullableType(inputStruct.Fields[index].second);
    }

    const auto* radix = FindKernel(
        kernels, op,
        anyNullableKey ? "sort.radix.nullable.fused" : "sort.radix.fused");
    if (embedWasm && radix &&
        !radix->Artifacts->Wasm.empty())
    {
        llvm::json::Array keys;
        for (size_t i = 0; i < radix->Kernel->SortKeys.size(); ++i) {
            const auto& key = radix->Kernel->SortKeys[i];
            const auto& sortKey = sortKeys[i];
            const bool nullsFirst = sortKey.Nulls == NQdb::ESortNulls::First ||
                (sortKey.Nulls == NQdb::ESortNulls::Default &&
                    sortKey.Direction == NQdb::ESortDirection::Desc);
            keys.push_back(llvm::json::Object{
                {"index", key.Index},
                {"width", key.WidthBytes},
                {"isString", key.IsString},
                {"desc", key.Desc},
                {"nullsFirst", nullsFirst},
            });
        }
        return llvm::json::Object{
            {"wasm", radix->Artifacts->Wasm},
            {"radixKeys", std::move(keys)},
            {"radixNullable", anyNullableKey},
        };
    }

    auto keys = ExecSortKeysJson(sortKeys, inputStruct);
    if (!keys) {
        return std::nullopt;
    }
    return llvm::json::Object{{"keys", std::move(*keys)}};
}

struct TExecGraphBuildResult {
    int64_t NodeId = -1;
    NQumir::NAst::TTypePtr OutputType;
};

struct TExecGraphBuilder {
    const TKernelIndex& Kernels;
    bool EmbedWasm = false;
    llvm::json::Array Nodes;
    llvm::json::Array Edges;
    int64_t NextNodeId = 0;
    std::optional<llvm::json::Object> Unsupported;
    std::optional<llvm::json::Object> Limit;

    int64_t AddNode(llvm::json::Object node) {
        const int64_t id = NextNodeId++;
        node["id"] = id;
        Nodes.push_back(std::move(node));
        return id;
    }

    void AddEdge(int64_t from, int64_t to, int64_t input) {
        Edges.push_back(llvm::json::Object{
            {"from", from},
            {"to", to},
            {"input", input},
            {"kind", "one-to-one"},
        });
    }

    // Keyless join = cross product (a scalar-subquery broadcast in practice).
    // The glue is pure column stitching; the residual predicate runs as a
    // plain filter node over the glued schema, reusing the kernel the lowerer
    // generated for the join operator.
    TExecGraphBuildResult BuildCrossJoin(
        NQdb::TJoinOperator& join,
        const TExecGraphBuildResult& left,
        const TExecGraphBuildResult& right,
        bool hasResidual)
    {
        using namespace NQdb;
        if (join.JoinType() != EJoinType::Inner) {
            Unsupported = UnsupportedExec(
                "browser keyless join type is not supported yet: " +
                std::string(JoinTypeName(join.JoinType())));
            return {};
        }
        auto outputType = ComputeJoinOutputType(
            left.OutputType, right.OutputType, EJoinType::Inner);
        if (!outputType) {
            Unsupported = UnsupportedExec(outputType.error().ToString());
            return {};
        }
        auto* leftStruct =
            static_cast<NQumir::NAst::TStructType*>(left.OutputType.get());
        auto* rightStruct =
            static_cast<NQumir::NAst::TStructType*>(right.OutputType.get());
        auto* outStruct =
            static_cast<NQumir::NAst::TStructType*>(outputType->get());
        const int64_t crossId = AddNode(llvm::json::Object{
            {"kind", "cross-join"},
            {"leftColumns", StructColumnsJson(*leftStruct)},
            {"rightColumns", StructColumnsJson(*rightStruct)},
            {"output", StructColumnsJson(*outStruct)},
        });
        AddEdge(left.NodeId, crossId, 0);
        AddEdge(right.NodeId, crossId, 1);
        if (!hasResidual) {
            return {.NodeId = crossId, .OutputType = *outputType};
        }
        const auto* ref = FindKernel(Kernels, &join, "filter");
        if (!ref) {
            Unsupported = UnsupportedExec(
                "cross join residual kernel was not generated");
            return {};
        }
        if (EmbedWasm && ref->Artifacts->Wasm.empty()) {
            Unsupported = UnsupportedExec(
                "cross join residual kernel failed to compile to wasm");
            return {};
        }
        const int64_t filterId = AddNode(llvm::json::Object{
            {"kind", "filter"},
            {"wasm", ref->Artifacts->Wasm},
        });
        AddEdge(crossId, filterId, 0);
        return {.NodeId = filterId, .OutputType = *outputType};
    }

    TExecGraphBuildResult Build(const NQdb::TOperatorPtr& op) {
        using namespace NQdb;
        if (Unsupported) {
            return {};
        }

        try {
            if (auto source = TMaybeOp<TSourceOperator>(op)) {
                auto outputType = BuildSourceRuntimeType(*source.Cast());
                const int64_t id = AddNode(llvm::json::Object{
                    {"kind", "source"},
                    {"table", source.Cast()->SourcePath()},
                    {"columns", SourceColumnsJson(outputType)},
                });
                return {.NodeId = id, .OutputType = outputType};
            }

            if (auto filter = TMaybeOp<TFilterOperator>(op)) {
                auto input = Build(filter.Cast()->Input());
                if (Unsupported) return {};
                const auto* ref = FindKernel(Kernels, filter.Cast().get(), "filter");
                if (!ref) {
                    Unsupported = UnsupportedExec("filter kernel was not generated");
                    return {};
                }
                if (EmbedWasm && ref->Artifacts->Wasm.empty()) {
                    Unsupported = UnsupportedExec("filter kernel failed to compile to wasm");
                    return {};
                }
                const int64_t id = AddNode(llvm::json::Object{
                    {"kind", "filter"},
                    {"wasm", ref->Artifacts->Wasm},
                });
                AddEdge(input.NodeId, id, 0);
                return {.NodeId = id, .OutputType = input.OutputType};
            }

            if (auto project = TMaybeOp<TProjectOperator>(op)) {
                auto input = Build(project.Cast()->Input());
                if (Unsupported) return {};
                auto* inputStruct =
                    static_cast<NQumir::NAst::TStructType*>(input.OutputType.get());
                auto columnPlan = BuildProjectColumnPlan(*project.Cast(), *inputStruct);
                std::string wasmId;
                if (!columnPlan.ComputedExprs.empty()) {
                    const auto* ref = FindKernel(Kernels, project.Cast().get(), "project");
                    if (!ref) {
                        Unsupported = UnsupportedExec("project kernel was not generated");
                        return {};
                    }
                    if (EmbedWasm && ref->Artifacts->Wasm.empty()) {
                        Unsupported = UnsupportedExec("project kernel failed to compile to wasm");
                        return {};
                    }
                    wasmId = ref->Artifacts->Wasm;
                }
                const int64_t id = AddNode(llvm::json::Object{
                    {"kind", "project"},
                    {"wasm", wasmId},
                    {"output", ProjectOutputJson(
                        *project.Cast(), *inputStruct, columnPlan.OutputType)},
                });
                AddEdge(input.NodeId, id, 0);
                return {.NodeId = id, .OutputType = columnPlan.OutputType};
            }

            if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
                auto input = Build(aggregate.Cast()->Input());
                if (Unsupported) return {};
                const auto* ref = FindKernel(Kernels, aggregate.Cast().get(), "aggregate");
                if (!ref) {
                    Unsupported = UnsupportedExec("aggregate kernel was not generated");
                    return {};
                }
                if (EmbedWasm && ref->Artifacts->Wasm.empty()) {
                    Unsupported = UnsupportedExec("aggregate kernel failed to compile to wasm");
                    return {};
                }
                auto outputType = ComputeAggregateOutputType(
                    input.OutputType,
                    aggregate.Cast()->GroupKeys(),
                    aggregate.Cast()->Aggs());
                auto* outStruct =
                    static_cast<NQumir::NAst::TStructType*>(outputType.get());
                const size_t keyCount = ref->Kernel->AggKeys.size();
                llvm::json::Array output;
                for (size_t i = 0; i < outStruct->Fields.size(); ++i) {
                    auto entry = CoreTypeJson(outStruct->Fields[i].second);
                    entry["name"] = BareColumnName(outStruct->Fields[i].first);
                    if (i < keyCount) {
                        entry["nullable"] = ref->Kernel->AggKeys[i].IsNullable;
                    } else if (i - keyCount < ref->Kernel->AggValues.size()) {
                        entry["nullable"] =
                            ref->Kernel->AggValues[i - keyCount].IsNullable;
                    }
                    output.push_back(std::move(entry));
                }
                const int64_t id = AddNode(llvm::json::Object{
                    {"kind", "aggregate"},
                    {"wasm", ref->Artifacts->Wasm},
                    {"keyCount", static_cast<int64_t>(keyCount)},
                    {"output", std::move(output)},
                });
                AddEdge(input.NodeId, id, 0);
                return {.NodeId = id, .OutputType = outputType};
            }

            if (auto join = TMaybeOp<TJoinOperator>(op)) {
                auto left = Build(join.Cast()->Left());
                auto right = Build(join.Cast()->Right());
                if (Unsupported) return {};
                const bool hasResidual = join.Cast()->Filter() != nullptr;
                if (join.Cast()->Keys().empty()) {
                    return BuildCrossJoin(
                        *join.Cast(), left, right, hasResidual);
                }
                if (!BrowserSupportsJoin(join.Cast()->JoinType(), hasResidual)) {
                    Unsupported = UnsupportedExec(
                        "browser join type is not supported yet: " +
                        std::string(JoinTypeName(join.Cast()->JoinType())) +
                        (hasResidual ? " with residual" : ""));
                    return {};
                }
                const auto* ref = FindKernel(Kernels, join.Cast().get(), "join");
                if (!ref) {
                    Unsupported = UnsupportedExec("join kernel was not generated");
                    return {};
                }
                if (EmbedWasm && ref->Artifacts->Wasm.empty()) {
                    Unsupported = UnsupportedExec("join kernel failed to compile to wasm");
                    return {};
                }
                auto outputType = ComputeJoinOutputType(
                    left.OutputType, right.OutputType, join.Cast()->JoinType());
                if (!outputType) {
                    Unsupported = UnsupportedExec(outputType.error().ToString());
                    return {};
                }
                auto* leftStruct =
                    static_cast<NQumir::NAst::TStructType*>(left.OutputType.get());
                auto* rightStruct =
                    static_cast<NQumir::NAst::TStructType*>(right.OutputType.get());
                auto* outStruct =
                    static_cast<NQumir::NAst::TStructType*>(outputType->get());
                llvm::json::Array keys;
                std::vector<std::pair<std::string, std::string>> keyPairs;
                for (const auto& key : join.Cast()->Keys()) {
                    keyPairs.emplace_back(key.Left, key.Right);
                    keys.push_back(llvm::json::Object{
                        {"left", BareColumnName(key.Left)},
                        {"right", BareColumnName(key.Right)},
                    });
                }
                const auto keyDesc =
                    NKernel::BuildJoinKeyDescriptor(*leftStruct, *rightStruct, keyPairs);
                const int64_t id = AddNode(llvm::json::Object{
                    {"kind", "join"},
                    {"joinType", std::string(JoinTypeName(join.Cast()->JoinType()))},
                    {"wasm", ref->Artifacts->Wasm},
                    {"keySize", static_cast<int64_t>(keyDesc.Size)},
                    {"hasResidual", hasResidual},
                    {"keys", std::move(keys)},
                    {"leftColumns", StructColumnsJson(*leftStruct)},
                    {"rightColumns", StructColumnsJson(*rightStruct)},
                    {"output", StructColumnsJson(*outStruct)},
                });
                AddEdge(left.NodeId, id, 0);
                AddEdge(right.NodeId, id, 1);
                return {.NodeId = id, .OutputType = *outputType};
            }

            if (auto sort = TMaybeOp<TSortOperator>(op)) {
                auto input = Build(sort.Cast()->Input());
                if (Unsupported) return {};
                auto* inputStruct =
                    static_cast<NQumir::NAst::TStructType*>(input.OutputType.get());
                auto stage = BuildSortStageJson(
                    sort.Cast()->Keys(), *inputStruct, Kernels,
                    sort.Cast().get(), EmbedWasm);
                if (!stage) {
                    Unsupported = UnsupportedExec("sort key references an unknown column");
                    return {};
                }
                stage->insert({"kind", "sort"});
                const int64_t id = AddNode(std::move(*stage));
                AddEdge(input.NodeId, id, 0);
                return {.NodeId = id, .OutputType = input.OutputType};
            }

            if (auto top = TMaybeOp<TTopSortOperator>(op)) {
                auto input = Build(top.Cast()->Input());
                if (Unsupported) return {};
                auto* inputStruct =
                    static_cast<NQumir::NAst::TStructType*>(input.OutputType.get());
                auto stage = BuildSortStageJson(
                    top.Cast()->Keys(), *inputStruct, Kernels,
                    top.Cast().get(), EmbedWasm);
                if (!stage) {
                    Unsupported = UnsupportedExec("top-sort key references an unknown column");
                    return {};
                }
                stage->insert({"kind", "top-sort"});
                stage->insert({"limit", top.Cast()->Limit()});
                const int64_t id = AddNode(std::move(*stage));
                AddEdge(input.NodeId, id, 0);
                return {.NodeId = id, .OutputType = input.OutputType};
            }

            if (auto limit = TMaybeOp<TLimitOperator>(op)) {
                auto input = Build(limit.Cast()->Input());
                if (Unsupported) return {};
                Limit = llvm::json::Object{
                    {"limit", limit.Cast()->Limit()},
                    {"offset", limit.Cast()->Offset()},
                };
                return input;
            }
        } catch (const NQumir::TError& e) {
            Unsupported = UnsupportedExec(e.ToString());
            return {};
        } catch (const std::exception& e) {
            Unsupported = UnsupportedExec(e.what());
            return {};
        }

        Unsupported = UnsupportedExec("plan contains an unsupported browser exec operator");
        return {};
    }
};

llvm::json::Object BuildExecGraphPlan(
    const NQdb::TOperatorPtr& plan,
    const TKernelIndex& kernels,
    bool embedWasm,
    size_t kernelCount)
{
    TExecGraphBuilder builder{.Kernels = kernels, .EmbedWasm = embedWasm};
    auto root = builder.Build(plan);
    if (builder.Unsupported) {
        return std::move(*builder.Unsupported);
    }
    llvm::json::Object out{
        {"supported", true},
        {"embedWasm", embedWasm},
        {"layout", ExecLayoutJson(kernelCount)},
        {"nodes", std::move(builder.Nodes)},
        {"edges", std::move(builder.Edges)},
        {"root", root.NodeId},
    };
    if (builder.Limit) {
        out["limit"] = std::move(*builder.Limit);
    }
    return out;
}

llvm::json::Object BuildExecPlan(
    const NQdb::TOperatorPtr& plan,
    const TKernelIndex& kernels,
    bool embedWasm,
    size_t kernelCount)
{
    return BuildExecGraphPlan(plan, kernels, embedWasm, kernelCount);
}

llvm::json::Object BuildBundle(TExportRequest& request) {
    auto plan = ParseSql(request.Sql, request.Catalog);
    if (!plan) {
        return ErrorObject("parse", plan.error().ToString());
    }

    try {
        ApplyPlanPasses(*plan);
    } catch (const std::exception& e) {
        return ErrorObject("logical", e.what());
    }

    std::ostringstream runtimeText;
    NQdb::PrintRuntimePlan(runtimeText, *plan);

    TArtifactStore artifacts;
    auto logicalGraph = LogicalGraphJson(*plan, artifacts);
    llvm::json::Object legacyGraph = EmptyGraphJson();
    llvm::json::Object physicalGraph = EmptyGraphJson();
    llvm::json::Array diagnostics;
    std::ostringstream schedulerText;
    std::vector<TKernelArtifacts> kernelArtifacts;
    std::vector<NQdb::TGeneratedKernel> loweredKernels;
    try {
        std::ostringstream lowerDiagnostics;
        // One lowering; kernels arrive as ASTs on lowered.Kernels.
        auto lowered = NQdb::NScheduler::LowerPlanToGraph(
            *plan,
            request.Scheduler,
            &lowerDiagnostics);
        lowered.Graph->Build();

        // Wasm finalization: ast artifacts always, wasm in embed mode.
        kernelArtifacts = WasmFinalizeKernels(
            lowered.Kernels, artifacts, diagnostics, request.EmbedWasm);
        loweredKernels = std::move(lowered.Kernels);

        schedulerText << lowerDiagnostics.str();
        schedulerText << "\n========== SCHEDULER GRAPH ==========\n";
        schedulerText << "mode=" << static_cast<int>(request.Scheduler.Scheduler.Mode)
                      << " workers=" << request.Scheduler.Scheduler.WorkerCount
                      << " scan_tasks=" << request.Scheduler.ScanSplit.MaxScanTasks
                      << "\n";
        lowered.Graph->Print(schedulerText);
        schedulerText << "=====================================\n";
        legacyGraph = GraphJson(lowered, kernelArtifacts);
        physicalGraph = GraphJson(lowered, kernelArtifacts);
    } catch (const std::exception& e) {
        const auto message = std::string(e.what());
        legacyGraph = EmptyGraphJson(message);
        physicalGraph = EmptyGraphJson(message);
        diagnostics.push_back(llvm::json::Object{
            {"stage", "lower"},
            {"message", message},
        });
    }

    // The exec section reads the kernels the lowering already generated —
    // nothing is compiled twice.
    auto kernelIndex = BuildKernelIndex(loweredKernels, kernelArtifacts);
    auto execPlan =
        BuildExecPlan(*plan, kernelIndex, request.EmbedWasm, kernelArtifacts.size());

    return llvm::json::Object{
            {"ok", true},
            {"format", "qdb.runtime.bundle"},
            {"version", 1},
            {"mode", SchedulerModeName(request.Scheduler.Scheduler.Mode)},
            {"dataset", llvm::json::Object{
                {"source", "browser"},
            }},
            {"exec", std::move(execPlan)},
            {"plans", llvm::json::Object{
                {"logicalText", LogicalPlanTreeText(*plan)},
                {"logicalAstText", LogicalPlanAstText(*plan)},
                {"physicalText", runtimeText.str() + schedulerText.str()},
            }},
            {"graph", std::move(legacyGraph)},
            {"graphs", llvm::json::Object{
                {"logical", std::move(logicalGraph)},
                {"physical", std::move(physicalGraph)},
            }},
            {"kernels", llvm::json::Object{}},
            {"modules", llvm::json::Object{}},
            {"artifacts", artifacts.Take()},
            {"diagnostics", std::move(diagnostics)},
    };
}

void PrintHelp(const char* argv0) {
    std::cout << "Usage: " << argv0 << " --stdin-json --stdout-json\n";
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;

    bool stdinJson = false;
    bool stdoutJson = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--stdin-json")) {
            stdinJson = true;
        } else if (!std::strcmp(argv[i], "--stdout-json")) {
            stdoutJson = true;
        } else if (!std::strcmp(argv[i], "--help")) {
            PrintHelp(argv[0]);
            return 0;
        }
    }

    if (!stdinJson || !stdoutJson) {
        std::cerr << "qdb_plan_export: expected --stdin-json --stdout-json\n";
        return 2;
    }

    auto request = ParseRequest(ReadAllStdin());
    if (!request) {
        return PrintError("request", request.error());
    }

    llvm::json::Object bundle;
    {
        TOutputSilencer silence;
        bundle = BuildBundle(*request);
    }

    std::cout << ToJsonString(std::move(bundle)) << "\n";
    return 0;
}
