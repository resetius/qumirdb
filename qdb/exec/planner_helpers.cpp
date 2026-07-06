#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/filter_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/project_type.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {
namespace {

// Byte width of the kernel output buffer per row for a computed column.
// For TStringType (string computed columns), the JIT kernel writes one
// TStringView struct (16 bytes) per row; the executor post-converts those.
size_t ProjectColumnWidth(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
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
        return 16; // sizeof(TStringView): ptr(8) + size(8)
    }
    throw std::runtime_error(
        "project: unsupported computed column type " +
        (type ? type->ToString() : std::string("<null>")));
}

// Returns the kernel-level type the project kernel should write for a computed
// column. StringView is resolved from the qumirdb source module before lowering.
NQumir::NAst::TTypePtr ProjectJitType(const NQumir::NAst::TTypePtr& outType) {
    using namespace NQumir::NAst;
    if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(outType)))) {
        return std::make_shared<TNamedType>("StringView", nullptr);
    }
    return outType;
}

bool IsRadixSortableType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    return static_cast<bool>(TMaybeType<TIntegerType>(inner)) ||
        static_cast<bool>(TMaybeType<TFloatType>(inner));
}

struct TSortKernelInputs {
    std::vector<TSortColumnRef> KeyColumns;
    std::vector<NQumir::NAst::TTypePtr> RadixTypes;
    bool AllKeysRadixSortable = true;
};

TSortKernelInputs BuildSortKernelInputs(const NKernel::TOperatorKernelSpec& spec) {
    TSortKernelInputs inputs;
    inputs.KeyColumns.reserve(spec.SortKeys.size());
    inputs.RadixTypes.reserve(spec.SortKeys.size());
    for (const auto& key : spec.SortKeys) {
        inputs.AllKeysRadixSortable =
            inputs.AllKeysRadixSortable && IsRadixSortableType(key.Column.Type);
        inputs.RadixTypes.push_back(key.Column.Type);
        inputs.KeyColumns.push_back({
            .Index = key.Column.Index,
            .Type = key.Column.Type,
        });
    }
    return inputs;
}

void PrintKernelSpec(std::ostream* out, const NKernel::TOperatorKernelSpec& spec) {
    if (!out) {
        return;
    }
    *out << "\n========== KERNEL SPEC ==========\n";
    NKernel::PrintKernelSpec(*out, spec);
    *out << "=================================\n";
}

} // namespace

NQumir::NAst::TTypePtr BuildSourceRuntimeType(TSourceOperator& src)
{
    if (auto required = src.RequiredColumns()) {
        auto* st = static_cast<NQumir::NAst::TStructType*>(required.get());
        std::unordered_set<std::string> cols;
        for (auto& [name, _] : st->Fields) {
            auto dot = name.rfind('.');
            cols.insert(dot != std::string::npos ? name.substr(dot + 1) : name);
        }
        src.GetSource().RestrictColumns(cols);
    }

    auto* qualSt = static_cast<NQumir::NAst::TStructType*>(
        src.OutputColumns().get());
    std::unordered_map<std::string,
        std::pair<std::string, NQumir::NAst::TTypePtr>> bareToQual;
    if (qualSt) {
        for (const auto& [qname, ftype] : qualSt->Fields) {
            auto dot = qname.rfind('.');
            auto bare = dot != std::string::npos ? qname.substr(dot + 1) : qname;
            bareToQual.try_emplace(bare, qname, ftype);
        }
    }

    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> fields;
    for (const auto& col : src.GetSource().Schema().Columns) {
        auto bare = std::string(col.Name);
        auto it = bareToQual.find(bare);
        if (it != bareToQual.end()) {
            fields.emplace_back(it->second.first, it->second.second);
        } else {
            fields.emplace_back(bare, col.Type);
        }
    }
    return std::make_shared<NQumir::NAst::TStructType>(std::move(fields));
}

TUnaryRuntimeProcess BuildFilterRuntimeProcess(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    std::ostream* diagnostics,
    IKernelExportBackend* exportBackend,
    std::string stage)
{
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("filter input must have TStructType");
    }

    auto spec = NKernel::BuildFilterKernelSpec(*inputStruct, filter.Predicate());
    TKernelCompiler compiler(TKernelCompilerOptions{
        .Diagnostics = diagnostics,
        .ExportBackend = exportBackend,
        .Stage = std::move(stage),
    });
    return {
        .Process = MakeFilterProcess(compiler.CompileFilter(spec)),
        .OutputType = inputType,
    };
}

TUnaryRuntimeProcess BuildProjectRuntimeProcess(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    std::ostream* diagnostics,
    IKernelExportBackend* exportBackend,
    std::string stage)
{
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("project input must have TStructType");
    }

    std::vector<TProjectColumn> columns;
    std::vector<NQumir::NAst::TExprPtr> computedExprs;
    std::vector<NQumir::NAst::TTypePtr> computedJitTypes;
    std::vector<size_t> computedWidths;
    std::vector<bool> computedIsString;
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
    for (const auto& projection : project.Projections()) {
        if (auto identNode = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
                projection.Expression)) {
            const std::string& exprName = identNode.Cast()->Name;
            auto it = std::find_if(
                inputStruct->Fields.begin(), inputStruct->Fields.end(),
                [&](const auto& field) { return field.first == exprName; });
            if (it == inputStruct->Fields.end()) {
                throw std::runtime_error("project column not found: " + exprName);
            }
            columns.push_back({
                .Computed = false,
                .Index = static_cast<int32_t>(
                    std::distance(inputStruct->Fields.begin(), it)),
            });
            outFields.emplace_back(projection.Name, it->second);
        } else {
            auto outType = NKernel::InferProjectExprType(
                projection.Expression,
                *inputStruct);
            auto jitType = ProjectJitType(outType);
            using namespace NQumir::NAst;
            bool isStr = static_cast<bool>(TMaybeType<TStringType>(
                UnwrapNamedType(UnwrapNullableType(outType))));
            columns.push_back({
                .Computed = true,
                .Index = static_cast<int32_t>(computedExprs.size()),
            });
            computedExprs.push_back(projection.Expression);
            computedJitTypes.push_back(jitType);
            computedWidths.push_back(ProjectColumnWidth(outType));
            computedIsString.push_back(isStr);
            outFields.emplace_back(projection.Name, outType);
        }
    }

    TKernelCompiler::TProjectDispatch dispatch;
    if (!computedExprs.empty()) {
        auto spec = NKernel::BuildProjectKernelSpec(
            *inputStruct,
            computedExprs,
            computedJitTypes);
        TKernelCompiler compiler(TKernelCompilerOptions{
            .Diagnostics = diagnostics,
            .ExportBackend = exportBackend,
            .Stage = std::move(stage),
        });
        dispatch = compiler.CompileProject(spec);
    }

    auto process = MakeProjectProcess(
        std::move(columns),
        std::move(dispatch),
        std::move(computedWidths),
        std::move(computedIsString));

    return {
        .Process = std::move(process),
        .OutputType = std::make_shared<NQumir::NAst::TStructType>(
            std::move(outFields)),
    };
}

TSortRuntimeProcess BuildSortRuntimeProcess(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string_view kernelName,
    std::ostream* diagnostics)
{
    auto spec = NKernel::BuildSortKernelSpec(inputType, keys, std::string(kernelName));
    PrintKernelSpec(diagnostics, spec);
    auto sortInputs = BuildSortKernelInputs(spec);
    TSortRadixKernel radixKernel;
    if (sortInputs.AllKeysRadixSortable && !sortInputs.RadixTypes.empty()) {
        TKernelCompiler compiler(diagnostics);
        radixKernel = {
            .Enabled = true,
            .Dispatch = compiler.CompileRadixSortComposite(sortInputs.RadixTypes),
            .NullableDispatch = compiler.CompileRadixSortCompositeNullable(
                sortInputs.RadixTypes),
        };
    }
    return {
        .KeyColumns = std::move(sortInputs.KeyColumns),
        .RadixKernel = std::move(radixKernel),
    };
}

namespace {

void PrintRuntimePlanNode(
    std::ostream& out, const TOperatorPtr& root, int depth)
{
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    out << indent;
    if (auto node = TMaybeOp<TSourceOperator>(root)) {
        out << "source " << node.Cast()->SourcePath() << "\n";
        return;
    }
    if (auto node = TMaybeOp<TFilterOperator>(root)) {
        out << "filter [JIT: AST -> IR -> LLVM]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TProjectOperator>(root)) {
        out << "project [column mapping]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TAggregateOperator>(root)) {
        out << "aggregate [JIT: update + finalize]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TJoinOperator>(root)) {
        out << "join [symmetric hash, JIT probe+insert]\n";
        PrintRuntimePlanNode(out, node.Cast()->Left(), depth + 1);
        PrintRuntimePlanNode(out, node.Cast()->Right(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TSortOperator>(root)) {
        out << "sort [stable indices]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TTopSortOperator>(root)) {
        out << "top-sort [bounded stable state]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TLimitOperator>(root)) {
        out << "limit [" << node.Cast()->Limit()
            << ", offset " << node.Cast()->Offset() << "]\n";
        PrintRuntimePlanNode(out, node.Cast()->Input(), depth + 1);
        return;
    }
    out << "unknown\n";
}

} // namespace

void PrintRuntimePlan(std::ostream& out, const TOperatorPtr& root) {
    out << "\n========== RUNTIME PLAN ==========\n";
    PrintRuntimePlanNode(out, root, 0);
    out << "==================================\n";
}

} // namespace NQdb
