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
#include <qdb/scheduler/plan_lowerer.h>
#include <qdb/scheduler/scan_split.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

namespace {

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
};

struct TKernelArtifacts {
    std::string Stage;
    std::string Name;
    std::string Ast;
    std::string Ir;
    std::string Llvm;
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

std::vector<TKernelArtifacts> ParseKernelArtifacts(
    std::string_view diagnostics,
    TArtifactStore& artifacts)
{
    static constexpr std::string_view stageHeader = "========== RUNTIME STAGE: ";
    static constexpr std::string_view stageHeaderEnd = " ==========";
    static constexpr std::string_view stageEnd =
        "========== END RUNTIME STAGE ==========";
    static constexpr std::string_view header = "========== RUNTIME NODE: ";
    static constexpr std::string_view headerEnd = " ==========";
    static constexpr std::string_view blockEnd =
        "========== END RUNTIME NODE ==========";

    std::vector<TKernelArtifacts> out;
    size_t pos = 0;
    std::string currentStage;
    while (true) {
        const auto stageBegin = diagnostics.find(stageHeader, pos);
        const auto stageFinish = diagnostics.find(stageEnd, pos);
        const auto nodeBegin = diagnostics.find(header, pos);
        auto next = std::min({stageBegin, stageFinish, nodeBegin});
        if (next == std::string_view::npos) {
            break;
        }

        if (next == stageBegin) {
            const auto valueBegin = stageBegin + stageHeader.size();
            const auto valueEnd = diagnostics.find(stageHeaderEnd, valueBegin);
            if (valueEnd == std::string_view::npos) {
                break;
            }
            currentStage = Trim(
                diagnostics.substr(valueBegin, valueEnd - valueBegin));
            pos = valueEnd + stageHeaderEnd.size();
            continue;
        }
        if (next == stageFinish) {
            currentStage.clear();
            pos = stageFinish + stageEnd.size();
            continue;
        }

        auto nameBegin = nodeBegin + header.size();
        auto nameEnd = diagnostics.find(headerEnd, nameBegin);
        if (nameEnd == std::string_view::npos) {
            break;
        }
        auto bodyBegin = diagnostics.find('\n', nameEnd);
        if (bodyBegin == std::string_view::npos) {
            break;
        }
        ++bodyBegin;
        auto end = diagnostics.find(blockEnd, bodyBegin);
        if (end == std::string_view::npos) {
            break;
        }

        const auto name = Trim(diagnostics.substr(nameBegin, nameEnd - nameBegin));
        const auto body = diagnostics.substr(bodyBegin, end - bodyBegin);
        TKernelArtifacts item{
            .Stage = currentStage,
            .Name = name,
        };
        auto ast = ArtifactSlice(body, "----- AST -----", "----- IR / LLVM -----");
        if (!ast.empty()) {
            item.Ast = artifacts.Add("ast", std::move(ast), name, currentStage);
        }

        auto ir = ArtifactSlice(body, "=========== IR: ============", "============================");
        if (!ir.empty()) {
            item.Ir = artifacts.Add("ir", std::move(ir), name, currentStage);
        }

        auto llvm = ArtifactSlice(body, "=========== LLVM: ==========", "============================");
        if (!llvm.empty()) {
            item.Llvm = artifacts.Add("llvm-ir", std::move(llvm), name, currentStage);
        }

        out.push_back(std::move(item));
        pos = end + blockEnd.size();
    }
    return out;
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
    try {
        std::ostringstream lowerDiagnostics;
        auto lowered = NQdb::NScheduler::LowerPlanToGraph(
            *plan,
            request.Scheduler,
            &lowerDiagnostics);
        lowered.Graph->Build();

        auto lowerDiagnosticsText = lowerDiagnostics.str();
        auto kernelArtifacts = ParseKernelArtifacts(lowerDiagnosticsText, artifacts);
        schedulerText << lowerDiagnosticsText;
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

    return llvm::json::Object{
            {"ok", true},
            {"format", "qdb.runtime.bundle"},
            {"version", 1},
            {"mode", SchedulerModeName(request.Scheduler.Scheduler.Mode)},
            {"dataset", llvm::json::Object{
                {"source", "browser"},
            }},
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
