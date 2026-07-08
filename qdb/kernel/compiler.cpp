#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/lib.h>
#include <qdb/kernel/spec.h>
#include <qdb/modules/qumirdb.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/error.h>
#include <qumir/parser/core/printer.h>

#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace NQdb {

namespace {

void PrintKernelAst(
    std::ostream* out,
    const std::string& nodeName,
    const NQumir::NAst::TExprPtr& ast)
{
    if (!out) {
        return;
    }
    *out << "\n========== RUNTIME NODE: " << nodeName << " ==========\n"
         << "----- AST -----\n";
    NQumir::NAst::NCore::PrintAst(*out, ast);
    *out << "\n----- IR / LLVM -----\n";
}

void FinishKernelDiagnostics(std::ostream* out) {
    if (out) {
        *out << "========== END RUNTIME NODE ==========\n";
    }
}

NQumir::NAst::TExprPtr ClonePredicate(
    const NQumir::NAst::TExprPtr& predicate)
{
    return CloneExpr(predicate);
}

void EnsureQumirDbUse(const NQumir::NAst::TExprPtr& ast) {
    using namespace NQumir::NAst;
    auto block = TMaybeNode<TBlockExpr>(ast);
    if (!block) {
        return;
    }
    for (const auto& stmt : block.Cast()->Stmts) {
        auto use = TMaybeNode<TUseExpr>(stmt);
        if (use && use.Cast()->ModuleName == "qumirdb") {
            return;
        }
    }
    block.Cast()->Stmts.insert(
        block.Cast()->Stmts.begin(),
        std::make_shared<TUseExpr>(NQumir::TLocation{}, "qumirdb"));
}

} // namespace

std::unordered_map<std::string, void*> CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    EnsureQumirDbUse(ast);
    return runner.CompileKernelAst(std::move(ast), entryNames, error);
}

TGeneratedKernel TKernelCompiler::EmitKernel(
    std::string name,
    std::vector<std::string> entrypoints,
    NQumir::NAst::TExprPtr ast,
    std::shared_ptr<void> storage)
{
    TGeneratedKernel kernel{
        .Name = std::move(name),
        .Stage = Stage_,
        .Entrypoints = std::move(entrypoints),
        .Ast = std::move(ast),
        .Storage = std::move(storage),
        .Operator = Operator_,
        .Slot = std::make_shared<TKernelSlot>(),
    };
    kernel.Slot->Fns.resize(kernel.Entrypoints.size(), nullptr);
    if (Sink_) {
        Sink_->push_back(kernel);
    }
    if (BindNow_) {
        JitFinalizeKernels(std::span(&kernel, 1), Diagnostics_);
    }
    return kernel;
}

NQumir::TLLVMRunnerOptions KernelRunnerOptions() {
    NQumir::TLLVMRunnerOptions opts;
    opts.CoreInput = true;
    opts.ResolveCoreInput = true;
    opts.AllowOverloads = true;
    opts.OptLevel = 3;
    opts.ModuleFiles = {std::string(QDB_SOURCE_DIR) + "/modules/qumirdb.oz"};
    return opts;
}

std::optional<std::string> CompileKernelAstToObject(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    EnsureQumirDbUse(ast);
    return runner.CompileKernelAstToObject(std::move(ast), entryNames, error);
}

namespace {

NQumir::NAst::TTypePtr QumirDbNamedType(const std::string& name) {
    return std::make_shared<NQumir::NAst::TNamedType>(name, nullptr);
}

std::string SortCoreTypeName(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return integer.Cast()->ToString();
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return "f64";
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return "u8";
    }
    return {};
}

int64_t SortRadixKeyBits(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return integer.Cast()->BitWidth();
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return 64;
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return 8;
    }
    return 0;
}

bool SortKeyIsString(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TStringType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

bool SortKeyIsBool(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TBoolType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

void EmitRowIdSortCall(
    std::ostringstream& out,
    const TSortRadixKeyInput& key,
    size_t keyIdx,
    bool nullable)
{
    const auto desc = "(index descs (: " + std::to_string(keyIdx) + " i64))";
    if (SortKeyIsString(key.Type)) {
        out << "      (call sort_string_rowids" << (nullable ? "_nullable" : "")
            << " store (: " << key.ColumnIndex << " i64)"
            << " row_ids (cast work <ptr u64>) counts n " << desc;
        if (nullable) {
            out << " (index nulls_firsts (: " << keyIdx << " i64))";
        }
        out << ")\n";
        return;
    }
    if (SortKeyIsBool(key.Type)) {
        out << "      (call sort_bool_rowids" << (nullable ? "_nullable" : "")
            << " store (: " << key.ColumnIndex << " i64)"
            << " row_ids work counts n " << desc;
        if (nullable) {
            out << " (index nulls_firsts (: " << keyIdx << " i64))";
        }
        out << ")\n";
        return;
    }

    const auto coreType = SortCoreTypeName(key.Type);
    const int64_t keyBits = SortRadixKeyBits(key.Type);
    if (coreType.empty() || keyBits == 0) {
        throw NQumir::TError(
            "CompileRadixSortComposite: unsupported key type " +
            (key.Type ? key.Type->ToString() : std::string("<null>")));
    }
    out << "      (call sort_fixed_rowids" << (nullable ? "_nullable" : "")
        << " store (: " << key.ColumnIndex << " i64)"
        << " row_ids work counts n (: " << keyBits << " i64) " << desc;
    if (nullable) {
        out << " (index nulls_firsts (: " << keyIdx << " i64))";
    }
    out << " (cast (: 0 i64) <ptr " << coreType << ">))\n";
}

std::string BuildRadixCompositeWrapperSource(
    const std::vector<TSortRadixKeyInput>& keys)
{
    std::ostringstream out;
    out << "(block\n";
    out
        << "  (fun qdb_radix_sort_indices_composite\n"
        << "       ((var store <ptr TRowSet>)\n"
        << "        (var row_ids <ptr i64>)\n"
        << "        (var work <ptr i64>)\n"
        << "        (var counts <ptr u32>)\n"
        << "        (var n i64)\n"
        << "        (var descs <ptr bool>))\n"
        << "    (block\n";

    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        EmitRowIdSortCall(out, keys[keyIdx], keyIdx, false);
    }

    out << "      )))\n";
    return out.str();
}

std::string BuildRadixCompositeNullableWrapperSource(
    const std::vector<TSortRadixKeyInput>& keys)
{
    std::ostringstream out;
    out << "(block\n";
    out
        << "  (fun qdb_radix_sort_indices_composite_nullable\n"
        << "       ((var store <ptr TRowSet>)\n"
        << "        (var row_ids <ptr i64>)\n"
        << "        (var work <ptr i64>)\n"
        << "        (var counts <ptr u32>)\n"
        << "        (var n i64)\n"
        << "        (var descs <ptr bool>)\n"
        << "        (var nulls_firsts <ptr bool>))\n"
        << "    (block\n";

    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        EmitRowIdSortCall(out, keys[keyIdx], keyIdx, true);
    }

    out << "      )))\n";
    return out.str();
}

} // namespace

NQumir::NAst::TExprPtr BuildRadixSortProgramAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("BuildRadixSortProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
        if (!library) {
            throw NQumir::TError(
                "BuildRadixSortProgramAst: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);
    addLibrary("sort_rowids.oz", true);
    auto wrapper = NKernel::ParseFunctionLibrary(BuildRadixCompositeWrapperSource(keys));
    if (!wrapper) {
        throw NQumir::TError("BuildRadixSortProgramAst: " + wrapper.error().ToString());
    }
    for (auto& stmt : *wrapper) {
        programStmts.push_back(std::move(stmt));
    }
    return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildRadixSortNullableProgramAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("BuildRadixSortNullableProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
        if (!library) {
            throw NQumir::TError(
                "BuildRadixSortNullableProgramAst: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);
    addLibrary("sort_rowids.oz", true);

    auto wrapper = NKernel::ParseFunctionLibrary(
        BuildRadixCompositeNullableWrapperSource(keys));
    if (!wrapper) {
        throw NQumir::TError(
            "BuildRadixSortNullableProgramAst: " + wrapper.error().ToString());
    }
    for (auto& stmt : *wrapper) {
        programStmts.push_back(std::move(stmt));
    }
    return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

TKernelCompiler::TFilterDispatch TKernelCompiler::CompileFilter(
    const NKernel::TOperatorKernelSpec& spec)
{
    using namespace NQumir::NAst;

    if (spec.InputSchemas.empty() || spec.Expressions.empty()) {
        throw std::runtime_error("filter kernel spec is incomplete");
    }
    auto inputType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    if (!inputType) {
        throw std::runtime_error("filter kernel input must have TStructType");
    }
    const auto& fields = inputType.Cast()->Fields;
    const auto& predicate = spec.Expressions[0];

    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(fields.size()); ++i) {
        fieldIndices[fields[i].first] = i;
    }

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto stringViewType = QumirDbNamedType("StringView");

    auto literalStorage =
        std::make_shared<std::vector<std::shared_ptr<std::string>>>();
    auto kernelAst = NKernel::BuildFilterProgramAst(
        ClonePredicate(predicate), *inputType.Cast(), fieldIndices, columnType,
        rowSetType, stringViewType, *literalStorage);
    if (!kernelAst) {
        throw NQumir::TError(
            "CompileFilter: " + kernelAst.error().ToString());
    }

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "filter", *kernelAst);

    auto kernel = EmitKernel(
        "filter", {"<kernel>"}, std::move(*kernelAst), literalStorage);
    FinishKernelDiagnostics(Diagnostics_);

    using TFilterFn = void(*)(void*);
    return [slot = kernel.Slot, storage = kernel.Storage](TRowSet& rowSet) {
        reinterpret_cast<TFilterFn>(slot->Fns[0])(&rowSet);
    };
}

TKernelCompiler::TProjectDispatch TKernelCompiler::CompileProject(
    const NKernel::TOperatorKernelSpec& spec)
{
    using namespace NQumir::NAst;

    if (spec.InputSchemas.empty()) {
        throw std::runtime_error("project kernel spec is missing input schema");
    }
    auto inputType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    if (!inputType) {
        throw std::runtime_error("project kernel input must have TStructType");
    }
    auto outputType = TMaybeType<TStructType>(spec.OutputSchema);
    if (!outputType) {
        throw std::runtime_error("project kernel output must have TStructType");
    }
    const auto& fields = inputType.Cast()->Fields;
    std::vector<NQumir::NAst::TTypePtr> computedTypes;
    computedTypes.reserve(outputType.Cast()->Fields.size());
    for (const auto& field : outputType.Cast()->Fields) {
        computedTypes.push_back(field.second);
    }

    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(fields.size()); ++i) {
        fieldIndices[fields[i].first] = i;
    }

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto stringViewType = QumirDbNamedType("StringView");

    auto literalStorage =
        std::make_shared<std::vector<std::shared_ptr<std::string>>>();
    std::vector<NQumir::NAst::TExprPtr> cloned;
    cloned.reserve(spec.Expressions.size());
    for (const auto& expr : spec.Expressions) {
        cloned.push_back(ClonePredicate(expr));
    }

    auto kernelAst = NKernel::GenProjectKernelAst(
        std::move(cloned), computedTypes, *inputType.Cast(), fieldIndices,
        columnType, rowSetType, stringViewType, *literalStorage);

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "project", kernelAst);

    auto kernel = EmitKernel(
        "project", {"<project>"}, std::move(kernelAst), literalStorage);
    FinishKernelDiagnostics(Diagnostics_);

    using TProjectFn = void(*)(void*, void**);
    return [slot = kernel.Slot, storage = kernel.Storage](
               TRowSet* in, void** outBuffers) {
        reinterpret_cast<TProjectFn>(slot->Fns[0])(in, outBuffers);
    };
}

TKernelCompiler::TSortRadixCompositeDispatch TKernelCompiler::CompileRadixSortComposite(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;

    if (keys.empty()) {
        throw NQumir::TError("CompileRadixSortComposite: empty key list");
    }

    auto program = BuildRadixSortProgramAst(keys);
    PrintKernelAst(Diagnostics_, "sort.radix.fused", program);

    auto kernel = EmitKernel(
        "sort.radix.fused",
        {"qdb_radix_sort_indices_composite"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using TSortFn = void(*)(TRowSet*, int64_t*, int64_t*, uint32_t*, int64_t, bool*);
    return [slot = kernel.Slot](TRowSet* store, int64_t* rowIds, int64_t* work,
        uint32_t* counts, int64_t n, bool* descs) {
        reinterpret_cast<TSortFn>(slot->Fns[0])(store, rowIds, work, counts, n, descs);
    };
}

TKernelCompiler::TSortRadixCompositeNullableDispatch
TKernelCompiler::CompileRadixSortCompositeNullable(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;

    auto program = BuildRadixSortNullableProgramAst(keys);
    PrintKernelAst(Diagnostics_, "sort.radix.nullable.fused", program);

    auto kernel = EmitKernel(
        "sort.radix.nullable.fused",
        {"qdb_radix_sort_indices_composite_nullable"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using TSortFn = void(*)(TRowSet*, int64_t*, int64_t*, uint32_t*,
        int64_t, bool*, bool*);
    return [slot = kernel.Slot](TRowSet* store, int64_t* rowIds,
        int64_t* work, uint32_t* counts, int64_t n, bool* descs, bool* nullsFirsts) {
        reinterpret_cast<TSortFn>(slot->Fns[0])(
            store, rowIds, work, counts, n, descs, nullsFirsts);
    };
}

TAggregateKernels TKernelCompiler::CompileAggregate(
    const NKernel::TOperatorKernelSpec& spec)
{
    if (spec.Kind != NKernel::EOperatorKernelKind::UnaryBlocking ||
        spec.OperatorName != "aggregate") {
        throw NQumir::TError("CompileAggregate: expected aggregate kernel spec");
    }
    if (spec.InputSchemas.size() != 1) {
        throw NQumir::TError("CompileAggregate: expected one input schema");
    }

    const auto* inputTypePtr = static_cast<NQumir::NAst::TStructType*>(
        spec.InputSchemas[0].get());
    if (!inputTypePtr) {
        throw NQumir::TError("CompileAggregate: input schema must be a struct");
    }
    const auto& inputType = *inputTypePtr;

    std::vector<std::string> groupKeys;
    if (!spec.Keys.empty()) {
        groupKeys.reserve(spec.Keys[0].Columns.size());
        for (const auto& column : spec.Keys[0].Columns) {
            groupKeys.push_back(column.Name);
        }
    }

    std::vector<TAggregateSpec> aggs;
    aggs.reserve(spec.Aggregates.size());
    for (const auto& agg : spec.Aggregates) {
        aggs.push_back({
            .Name = agg.Name,
            .Func = agg.Func,
            .Arg = agg.ArgExpr,
        });
    }

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    using namespace NQumir::NAst;

    auto fieldType = [&](const std::string& name) -> TTypePtr {
        for (const auto& [n, t] : inputType.Fields) {
            if (n == name) {
                return t;
            }
        }
        return nullptr;
    };
    auto requireField = [&](const std::string& name) -> TTypePtr {
        auto type = fieldType(name);
        if (!type) {
            throw NQumir::TError("CompileAggregate: unknown column '" + name + "'");
        }
        return type;
    };
    const auto keyDescriptor = NKernel::BuildAggregateKeyDescriptor(inputType, groupKeys);
    for (const auto& field : keyDescriptor.Fields) {
        const auto type = UnwrapNamedType(UnwrapNullableType(field.Type));
        if (!TMaybeType<TIntegerType>(type) && !TMaybeType<TFloatType>(type) &&
            !TMaybeType<TStringType>(type)) {
            throw NQumir::TError(
                "CompileAggregate: group key column '" + field.ColumnName +
                "' must be integer, f64, or string");
        }
    }
    auto columnIndex = [&](const std::string& name) -> int32_t {
        for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
            if (inputType.Fields[i].first == name) {
                return i;
            }
        }
        return -1;
    };

    std::vector<std::string> funcs;
    std::vector<NKernel::TAggArg> args;
    funcs.reserve(aggs.size());
    args.reserve(aggs.size());
    for (const auto& agg : aggs) {
        if (agg.Func != "count" && agg.Func != "sum" && agg.Func != "min" && agg.Func != "max") {
            throw NQumir::TError("CompileAggregate: unsupported aggregate function '" + agg.Func + "'");
        }
        funcs.push_back(agg.Func);

        if (agg.Func != "count" && !agg.Arg) {
            throw NQumir::TError(
                "CompileAggregate: '" + agg.Func + "' requires an argument column");
        }
        NKernel::TAggArg arg;
        if (agg.Arg) {
            auto ident = TMaybeNode<TIdentExpr>(agg.Arg);
            if (!ident) {
                throw NQumir::TError("CompileAggregate: aggregate argument must be a column reference");
            }
            const std::string& name = ident.Cast()->Name;
            const auto type = requireField(name);
            const auto unwrapped = UnwrapNamedType(UnwrapNullableType(type));
            arg.IsFloat = static_cast<bool>(TMaybeType<TFloatType>(unwrapped));
            if (!TMaybeType<TIntegerType>(unwrapped) && !arg.IsFloat) {
                throw NQumir::TError(
                    "CompileAggregate: aggregate argument column '" + name +
                    "' must be integer or f64");
            }
            arg.IsNullable = IsNullableType(type);
            if (arg.IsFloat && arg.IsNullable) {
                throw NQumir::TError(
                    "CompileAggregate: nullable f64 aggregates are not supported yet");
            }
            arg.ColumnIndex = columnIndex(name);
        }
        args.push_back(arg);
    }
    const auto layout = NKernel::BuildAggReducerLayout(funcs, args);

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto hashTableType = QumirDbNamedType("HashTable");

    auto program = NKernel::BuildGenericAggregateFusedProgramAst(
        inputType, keyDescriptor, layout,
        columnType, rowSetType, hashTableType);
    if (!program) {
        throw NQumir::TError(
            "CompileAggregate: " + program.error().ToString());
    }

    TAggregateKernels kernels;
    kernels.NumAggs = funcs.size();
    kernels.OutputAggs.reserve(layout.Reducers.size());
    for (const auto& reducer : layout.Reducers) {
        kernels.OutputAggs.push_back({.IsNullable = reducer.IsNullableOutput});
    }
    kernels.OutputKeys.reserve(keyDescriptor.Fields.size());
    for (const auto& field : keyDescriptor.Fields) {
        const auto logicalType = UnwrapNamedType(field.Type);
        kernels.OutputKeys.push_back({
            .Kind = TMaybeType<TStringType>(logicalType)
                ? EAggregateOutputKeyKind::String
                : EAggregateOutputKeyKind::Fixed,
            .IsNullable = field.IsNullable,
            .Size = field.Size,
            .Alignment = field.Alignment,
        });
    }

    const size_t sinkBefore = Sink_ ? Sink_->size() : 0;
    PrintKernelAst(Diagnostics_, "aggregate", *program);
    auto kernel = EmitKernel(
        "aggregate",
        {"agg_dispatch", "agg_measure_keys", "agg_finalize"},
        std::move(*program));
    FinishKernelDiagnostics(Diagnostics_);

    if (Sink_) {
        // Output-layout metadata for the exec exporter.
        std::vector<TGeneratedKernel::TAggKeyMeta> keyMeta;
        keyMeta.reserve(kernels.OutputKeys.size());
        for (const auto& outputKey : kernels.OutputKeys) {
            keyMeta.push_back({
                .IsString = outputKey.Kind == EAggregateOutputKeyKind::String,
                .IsNullable = outputKey.IsNullable,
            });
        }
        std::vector<TGeneratedKernel::TAggValueMeta> valueMeta;
        valueMeta.reserve(kernels.OutputAggs.size());
        for (const auto& outputAgg : kernels.OutputAggs) {
            valueMeta.push_back({.IsNullable = outputAgg.IsNullable});
        }
        for (size_t i = sinkBefore; i < Sink_->size(); ++i) {
            (*Sink_)[i].AggKeys = keyMeta;
            (*Sink_)[i].AggValues = valueMeta;
        }
    }

    using TDispatchFn = int64_t(*)(void*, TRowSet*, int64_t, int64_t);
    using TMeasureFn = int64_t(*)(void*, int64_t*, int64_t);
    using TFinalizeFn = int64_t(*)(void*, void**, int64_t**, uint8_t**, int64_t);

    auto slot = kernel.Slot;
    kernels.Dispatch = [slot](void* ht, TRowSet* batch, int64_t arg, int64_t op) {
        return reinterpret_cast<TDispatchFn>(slot->Fns[0])(ht, batch, arg, op);
    };
    kernels.Measure = [slot](void* ht, int64_t* outputKeyBytes, int64_t outputCapacity) {
        return reinterpret_cast<TMeasureFn>(slot->Fns[1])(ht, outputKeyBytes, outputCapacity);
    };
    kernels.Finalize = [slot](void* ht, void** outputKeyBuffers, int64_t** outputBuffers, uint8_t** outputAggMasks, int64_t outputCapacity) {
        return reinterpret_cast<TFinalizeFn>(slot->Fns[2])(ht, outputKeyBuffers, outputBuffers, outputAggMasks, outputCapacity);
    };
    return kernels;
}

TJoinKernels TKernelCompiler::CompileJoin(
    const NKernel::TOperatorKernelSpec& spec)
{
    using namespace NQumir::NAst;

    if (spec.Kind != NKernel::EOperatorKernelKind::Binary ||
        spec.OperatorName != "join") {
        throw NQumir::TError("CompileJoin: expected join kernel spec");
    }
    if (spec.InputSchemas.size() != 2) {
        throw NQumir::TError("CompileJoin: expected two input schemas");
    }

    auto leftType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    auto rightType = TMaybeType<TStructType>(spec.InputSchemas[1]);
    if (!leftType || !rightType) {
        throw NQumir::TError("CompileJoin: input schemas must be structs");
    }

    std::vector<std::pair<std::string, std::string>> keys;
    keys.reserve(spec.JoinKeys.size());
    for (const auto& key : spec.JoinKeys) {
        keys.emplace_back(key.Left.Name, key.Right.Name);
    }

    const TExprPtr residualPredicate =
        spec.Expressions.empty() ? nullptr : spec.Expressions[0];
    if (residualPredicate &&
        spec.JoinType != EJoinType::Inner &&
        spec.JoinType != EJoinType::LeftSemi &&
        spec.JoinType != EJoinType::LeftAnti) {
        throw NQumir::TError(
            "CompileJoin: residual filter is not yet supported for this join type");
    }

    TTypePtr innerOutputType;
    TStructType* innerType = nullptr;
    size_t leftFieldCount = 0;
    if (residualPredicate) {
        auto innerOutput = ComputeJoinOutputType(
            spec.InputSchemas[0], spec.InputSchemas[1], EJoinType::Inner);
        if (!innerOutput) {
            throw NQumir::TError(
                "CompileJoin: residual filter: " + innerOutput.error().ToString());
        }
        innerOutputType = *innerOutput;
        innerType = static_cast<TStructType*>(innerOutputType.get());
        leftFieldCount = leftType.Cast()->Fields.size();
    }

    const EJoinType kernelType =
        (residualPredicate &&
         (spec.JoinType == EJoinType::LeftSemi ||
          spec.JoinType == EJoinType::LeftAnti))
        ? EJoinType::Inner
        : spec.JoinType;

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    return CompileJoin(
        *leftType.Cast(), *rightType.Cast(), keys, kernelType,
        residualPredicate, innerType, leftFieldCount);
}

TJoinHashKernels TKernelCompiler::CompileJoinHash(
    const NKernel::TOperatorKernelSpec& spec)
{
    using namespace NQumir::NAst;

    if (spec.Kind != NKernel::EOperatorKernelKind::Binary ||
        spec.OperatorName != "join") {
        throw NQumir::TError("CompileJoinHash: expected join kernel spec");
    }
    if (spec.InputSchemas.size() != 2) {
        throw NQumir::TError("CompileJoinHash: expected two input schemas");
    }

    auto leftType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    auto rightType = TMaybeType<TStructType>(spec.InputSchemas[1]);
    if (!leftType || !rightType) {
        throw NQumir::TError("CompileJoinHash: input schemas must be structs");
    }

    std::vector<std::pair<std::string, std::string>> keys;
    keys.reserve(spec.JoinKeys.size());
    for (const auto& key : spec.JoinKeys) {
        keys.emplace_back(key.Left.Name, key.Right.Name);
    }
    const auto keyDesc = NKernel::BuildJoinKeyDescriptor(
        *leftType.Cast(),
        *rightType.Cast(),
        keys);

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");

    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        program.push_back(NKernel::GenJoinHashBatchAst(
            keyDesc,
            /*isLeft=*/true,
            "jt_hash_left",
            columnType,
            rowSetType));
        program.push_back(NKernel::GenJoinHashBatchAst(
            keyDesc,
            /*isLeft=*/false,
            "jt_hash_right",
            columnType,
            rowSetType));
        return program;
    };

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "join_hash", program);

    auto kernel = EmitKernel(
        "join_hash", {"jt_hash_left", "jt_hash_right"}, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using THashFn = bool(*)(TRowSet*, uint64_t*);
    return {
        .Left = [slot = kernel.Slot](TRowSet* batch, uint64_t* hashes) {
            return reinterpret_cast<THashFn>(slot->Fns[0])(batch, hashes);
        },
        .Right = [slot = kernel.Slot](TRowSet* batch, uint64_t* hashes) {
            return reinterpret_cast<THashFn>(slot->Fns[1])(batch, hashes);
        },
    };
}

TJoinKernels TKernelCompiler::CompileJoin(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<std::pair<std::string, std::string>>& keys,
    EJoinType type,
    const NQumir::NAst::TExprPtr& residualPredicate,
    const NQumir::NAst::TStructType* innerType,
    size_t leftFieldCount)
{
    using namespace NQumir::NAst;

    const bool isSemiAnti = (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti);
    const bool isOuter = (type == EJoinType::Left || type == EJoinType::Right);
    if (!isSemiAnti && !isOuter && type != EJoinType::Inner) {
        throw NQumir::TError(
            "CompileJoin: only Inner, Left, Right, LeftSemi, and LeftAnti join types are supported");
    }

    // Unified key descriptor (reuses the aggregation key machinery). Throws on
    // incompatible types / missing columns; string keys are rejected by
    // GenJoinProcessAst below.
    const auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, keys);
    const int64_t keySize = static_cast<int64_t>(keyDesc.Size);

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto hashTableType = QumirDbNamedType("HashTable");
    auto pairBufferType = QumirDbNamedType("PairBuffer");
    auto stringViewType = QumirDbNamedType("StringView");

    // Fresh program per entry (CompileKernelAst consumes the AST): key type
    // decls + key-ops overloads + shared library + generated process functions.
    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        auto library = NKernel::BuildJoinKernelLibrary();
        if (!library) {
            throw NQumir::TError("CompileJoin: " + library.error().ToString());
        }
        // When a residual predicate is given, replace the library's default
        // jt_residual_filter (join_residual_default.oz) with the generated one,
        // keeping its position (before join_update.oz, which calls it).
        if (residualPredicate && innerType) {
            for (auto& f : *library) {
                auto fun = NQumir::NAst::TMaybeNode<NQumir::NAst::TFunDecl>(f);
                if (fun && fun.Cast()->Name == "jt_residual_filter") {
                    f = NKernel::GenJoinResidualFilterAst(
                        residualPredicate, *innerType, leftFieldCount,
                        columnType, rowSetType, stringViewType);
                    break;
                }
            }
        }

        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) program.push_back(std::move(f));
        for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) program.push_back(std::move(f));
        for (auto& f : *library) program.push_back(std::move(f));
        program.push_back(NKernel::GenJoinProcessAst(keyDesc, /*isLeft=*/true,
            "jt_process_left", columnType, rowSetType, hashTableType, pairBufferType));
        program.push_back(NKernel::GenJoinProcessAst(keyDesc, /*isLeft=*/false,
            "jt_process_right", columnType, rowSetType, hashTableType, pairBufferType));
        program.push_back(NKernel::GenJoinProbeAst(keyDesc, /*isLeft=*/true,
            "jt_probe_left_stream", columnType, rowSetType, hashTableType, pairBufferType));
        program.push_back(NKernel::GenJoinProbeAst(keyDesc, /*isLeft=*/false,
            "jt_probe_right_stream", columnType, rowSetType, hashTableType, pairBufferType));
        if (isSemiAnti) {
            program.push_back(NKernel::GenJoinInsertKeyOnlyAst(
                keyDesc, "jt_insert_key_only",
                columnType, rowSetType, hashTableType, pairBufferType));
            program.push_back(NKernel::GenJoinFinalizeSemiAntiAst(
                keyDesc, /*isAnti=*/type == EJoinType::LeftAnti,
                "jt_finalize_semi_anti", hashTableType, pairBufferType));
        }
        if (isOuter) {
            // jt_finalize_outer: same logic as ANTI (emit own rows with no opp match)
            program.push_back(NKernel::GenJoinFinalizeSemiAntiAst(
                keyDesc, /*isAnti=*/true,
                "jt_finalize_outer", hashTableType, pairBufferType));
        }
        return program;
    };

    std::vector<std::string> entries = {
        "jt_init",
        "jt_process_left",
        "jt_process_right",
        "jt_probe_left_stream",
        "jt_probe_right_stream",
        "jt_destroy",
        "pb_destroy",
    };
    if (isSemiAnti) {
        entries.push_back("jt_insert_key_only");
        entries.push_back("jt_finalize_semi_anti");
    }
    if (isOuter) {
        entries.push_back("jt_finalize_outer");
    }

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "join", program);

    auto kernel = EmitKernel("join", entries, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    // Entry indices are fixed by the `entries` order above.
    auto slot = kernel.Slot;
    auto entryIndex = [&](std::string_view name) -> size_t {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i] == name) {
                return i;
            }
        }
        throw std::logic_error("CompileJoin: missing entry " + std::string(name));
    };

    using TInitFn = bool(*)(void*, int64_t, int64_t);
    // jt_process_left/right take the two row-store bases (for jt_residual_filter).
    using TProcessFn = bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*);
    using TProbeFn = bool(*)(void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*);
    // jt_insert_key_only keeps the original 5-arg ABI (no residual filter).
    using TInsertKeyOnlyFn = bool(*)(void*, void*, TRowSet*, int64_t, void*);
    using TDestroyFn = void(*)(void*);

    TJoinKernels kernels;
    kernels.Init = [slot, i = entryIndex("jt_init"), keySize](
        void* table, int64_t capacity) {
        return reinterpret_cast<TInitFn>(slot->Fns[i])(table, capacity, keySize);
    };
    kernels.ProcessLeft = [slot, i = entryIndex("jt_process_left")](
        void* own, void* opp, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProcessFn>(slot->Fns[i])(
            own, opp, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.ProcessRight = [slot, i = entryIndex("jt_process_right")](
        void* own, void* opp, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProcessFn>(slot->Fns[i])(
            own, opp, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.ProbeLeftStream = [slot, i = entryIndex("jt_probe_left_stream")](
        void* build, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProbeFn>(slot->Fns[i])(
            build, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.ProbeRightStream = [slot, i = entryIndex("jt_probe_right_stream")](
        void* build, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProbeFn>(slot->Fns[i])(
            build, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.DestroyTable = [slot, i = entryIndex("jt_destroy")](void* table) {
        reinterpret_cast<TDestroyFn>(slot->Fns[i])(table);
    };
    kernels.DestroyPairs = [slot, i = entryIndex("pb_destroy")](void* pairs) {
        reinterpret_cast<TDestroyFn>(slot->Fns[i])(pairs);
    };

    using TFinalizeFn = bool(*)(void*, void*, void*);

    if (isSemiAnti) {
        kernels.InsertKeyOnly = [slot, i = entryIndex("jt_insert_key_only")](
            void* own, void* opp, TRowSet* batch,
            int64_t batchIdx, void* pairs) {
            return reinterpret_cast<TInsertKeyOnlyFn>(slot->Fns[i])(
                own, opp, batch, batchIdx, pairs);
        };
        kernels.FinalizeAntiSemi = [slot, i = entryIndex("jt_finalize_semi_anti")](
            void* own, void* opp, void* pairs) {
            return reinterpret_cast<TFinalizeFn>(slot->Fns[i])(own, opp, pairs);
        };
    }

    if (isOuter) {
        kernels.FinalizeOuter = [slot, i = entryIndex("jt_finalize_outer")](
            void* own, void* opp, void* pairs) {
            return reinterpret_cast<TFinalizeFn>(slot->Fns[i])(own, opp, pairs);
        };
    }

    return kernels;
}

} // namespace NQdb
