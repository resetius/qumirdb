#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/filter_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/types/decimal.h>
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
    if (IsDecimalType(type)) {
        return 16;
    }
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
    if (IsDecimalType(outType)) {
        return DecimalStorageTypeFor(outType);
    }
    if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(outType)))) {
        TTypePtr sv = std::make_shared<TNamedType>("StringView", nullptr);
        // Keep nullability so the project kernel emits the Nullable[StringView] split
        // (Value -> data, Valid -> mask) instead of casting the struct to StringView.
        return IsNullableType(outType) ? std::make_shared<TNullable>(std::move(sv)) : sv;
    }
    return outType;
}

bool IsRadixSortableType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    if (DecimalSpecOfValueType(UnwrapNullableType(type))) {
        return true; // 128-bit decimal: two-word radix (sort_binint_rowids)
    }
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    return static_cast<bool>(TMaybeType<TIntegerType>(inner)) ||
        static_cast<bool>(TMaybeType<TFloatType>(inner)) ||
        static_cast<bool>(TMaybeType<TBoolType>(inner)) ||
        static_cast<bool>(TMaybeType<TStringType>(inner));
}

bool SortNullsFirst(const NKernel::TKernelSortKeySpec& key) {
    if (key.Nulls != ESortNulls::Default) {
        return key.Nulls == ESortNulls::First;
    }
    return key.Direction == ESortDirection::Desc;
}

struct TSortKernelInputs {
    std::vector<TSortColumnRef> KeyColumns;
    std::vector<TSortRadixKeyInput> RadixKeys;
    bool AllKeysRadixSortable = true;
};

TSortKernelInputs BuildSortKernelInputs(const NKernel::TOperatorKernelSpec& spec) {
    TSortKernelInputs inputs;
    inputs.KeyColumns.reserve(spec.SortKeys.size());
    inputs.RadixKeys.reserve(spec.SortKeys.size());
    for (const auto& key : spec.SortKeys) {
        inputs.AllKeysRadixSortable =
            inputs.AllKeysRadixSortable && IsRadixSortableType(key.Column.Type);
        inputs.RadixKeys.push_back({
            .ColumnIndex = key.Column.Index,
            .Type = key.Column.Type,
            .Desc = key.Direction == ESortDirection::Desc,
            .NullsFirst = SortNullsFirst(key),
        });
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
    TKernelCompilerOptions options)
{
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("filter input must have TStructType");
    }

    auto spec = NKernel::BuildFilterKernelSpec(*inputStruct, filter.Predicate());
    TKernelCompiler compiler(std::move(options));
    return {
        .Process = MakeFilterProcess(compiler.CompileFilter(spec)),
        .OutputType = inputType,
    };
}

TProjectColumnPlan BuildProjectColumnPlan(
    TProjectOperator& project,
    const NQumir::NAst::TStructType& inputStruct)
{
    TProjectColumnPlan plan;
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
    for (const auto& projection : project.Projections()) {
        if (auto identNode = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
                projection.Expression)) {
            const std::string& exprName = identNode.Cast()->Name;
            auto it = std::find_if(
                inputStruct.Fields.begin(), inputStruct.Fields.end(),
                [&](const auto& field) { return field.first == exprName; });
            if (it == inputStruct.Fields.end()) {
                throw std::runtime_error("project column not found: " + exprName);
            }
            plan.Columns.push_back({
                .Computed = false,
                .Index = static_cast<int32_t>(
                    std::distance(inputStruct.Fields.begin(), it)),
            });
            outFields.emplace_back(projection.Name, it->second);
        } else {
            auto outType = NKernel::AnnotateExprType(
                projection.Expression,
                inputStruct);
            auto jitType = ProjectJitType(outType);
            using namespace NQumir::NAst;
            bool isStr = static_cast<bool>(TMaybeType<TStringType>(
                UnwrapNamedType(UnwrapNullableType(outType))));
            plan.Columns.push_back({
                .Computed = true,
                .Index = static_cast<int32_t>(plan.ComputedExprs.size()),
            });
            plan.ComputedExprs.push_back(projection.Expression);
            plan.ComputedJitTypes.push_back(jitType);
            plan.ComputedWidths.push_back(ProjectColumnWidth(outType));
            plan.ComputedIsString.push_back(isStr);
            plan.ComputedIsNullable.push_back(IsNullableType(outType));
            outFields.emplace_back(projection.Name, outType);
        }
    }
    plan.OutputType = std::make_shared<NQumir::NAst::TStructType>(
        std::move(outFields));
    return plan;
}

TUnaryRuntimeProcess BuildProjectRuntimeProcess(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options)
{
    auto* inputStruct = static_cast<NQumir::NAst::TStructType*>(inputType.get());
    if (!inputStruct) {
        throw std::runtime_error("project input must have TStructType");
    }

    auto plan = BuildProjectColumnPlan(project, *inputStruct);

    TKernelCompiler::TProjectDispatch dispatch;
    if (!plan.ComputedExprs.empty()) {
        auto spec = NKernel::BuildProjectKernelSpec(
            *inputStruct,
            plan.ComputedExprs,
            plan.ComputedJitTypes);
        TKernelCompiler compiler(std::move(options));
        dispatch = compiler.CompileProject(spec);
    }

    auto process = MakeProjectProcess(
        std::move(plan.Columns),
        std::move(dispatch),
        std::move(plan.ComputedWidths),
        std::move(plan.ComputedIsString),
        std::move(plan.ComputedIsNullable));

    return {
        .Process = std::move(process),
        .OutputType = std::move(plan.OutputType),
    };
}

// Radix key byte width: integer width, or 8 for f64.
int32_t RadixKeyWidthBytes(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    if (DecimalSpecOfValueType(UnwrapNullableType(type))) {
        return 16; // qdb_bin_int (128-bit)
    }
    auto inner = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(inner)) {
        return static_cast<int32_t>(integer.Cast()->BitWidth() / 8);
    }
    if (TMaybeType<TFloatType>(inner)) {
        return 8;
    }
    if (TMaybeType<TBoolType>(inner)) {
        return 1;
    }
    return 0;
}

bool IsStringRadixKey(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TStringType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

TSortRuntimeProcess BuildSortRuntimeProcess(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string_view kernelName,
    TKernelCompilerOptions options)
{
    auto spec = NKernel::BuildSortKernelSpec(inputType, keys, std::string(kernelName));
    PrintKernelSpec(options.Diagnostics, spec);
    auto sortInputs = BuildSortKernelInputs(spec);
    TSortRadixKernel radixKernel;
    if (!sortInputs.AllKeysRadixSortable || sortInputs.RadixKeys.empty()) {
        throw std::runtime_error("sort kernel is unavailable for requested keys");
    }
    {
        auto* sink = options.Sink;
        const size_t sinkBefore = sink ? sink->size() : 0;
        // The nullable variant is only reachable when a key column can carry a
        // null mask; with all keys non-nullable by schema it is dead code, so
        // skip generating it.
        bool anyNullableKey = false;
        for (const auto& key : sortInputs.RadixKeys) {
            anyNullableKey = anyNullableKey || IsNullableType(key.Type);
        }
        auto radixOptions = options;
        if (kernelName == "top-sort") {
            radixOptions.Diagnostics = nullptr;
        }
        TKernelCompiler compiler(std::move(radixOptions));
        radixKernel = {
            .Enabled = true,
            .Dispatch = anyNullableKey
                ? TKernelCompiler::TSortRadixCompositeDispatch{}
                : compiler.CompileRadixSortComposite(sortInputs.RadixKeys, &inputType),
            .NullableDispatch = anyNullableKey
                ? compiler.CompileRadixSortCompositeNullable(sortInputs.RadixKeys, &inputType)
                : TKernelCompiler::TSortRadixCompositeNullableDispatch{},
        };
        if (kernelName == "top-sort") {
            if (sink) {
                for (size_t i = sinkBefore; i < sink->size(); ++i) {
                    (*sink)[i].ExportArtifacts = false;
                }
            }
            TKernelCompiler topSortCompiler(std::move(options));
            radixKernel.TopSortDispatch =
                topSortCompiler.CompileTopSort(sortInputs.RadixKeys, &inputType);
        }
        if (sink) {
            // Resolved key metadata for the exec exporter, on both emitted
            // sort kernels.
            std::vector<TGeneratedKernel::TSortKeyMeta> meta;
            meta.reserve(keys.size());
            for (size_t k = 0; k < sortInputs.KeyColumns.size(); ++k) {
                meta.push_back({
                    .Index = sortInputs.KeyColumns[k].Index,
                    .WidthBytes = RadixKeyWidthBytes(sortInputs.KeyColumns[k].Type),
                    .IsString = IsStringRadixKey(sortInputs.KeyColumns[k].Type),
                    .Desc = keys[k].Direction == ESortDirection::Desc,
                });
            }
            for (size_t i = sinkBefore; i < sink->size(); ++i) {
                (*sink)[i].SortKeys = meta;
            }
        }
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
