#include <qdb/kernel/compiler.h>
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

#include <optional>
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

void* CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::string& entryName,
    std::string* error)
{
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    EnsureQumirDbUse(ast);
    return runner.CompileKernelAst(std::move(ast), entryName, error);
}

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
    return 0;
}

std::string BuildRadixCompositeWrapperSource(
    const std::vector<NQumir::NAst::TTypePtr>& types)
{
    std::ostringstream out;
    out << "(block\n"
        << "  (fun qdb_radix_sort_indices_composite\n"
        << "       ((var values <ptr <ptr i8>>)\n"
        << "        (var indices <ptr u32>)\n"
        << "        (var work <ptr u32>)\n"
        << "        (var counts <ptr u32>)\n"
        << "        (var n i64)\n"
        << "        (var descs <ptr bool>))\n"
        << "    (block\n";

    for (size_t k = types.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        const auto coreType = SortCoreTypeName(types[keyIdx]);
        const int64_t keyBits = SortRadixKeyBits(types[keyIdx]);
        if (coreType.empty() || keyBits == 0) {
            throw NQumir::TError(
                "CompileRadixSortComposite: unsupported key type " +
                (types[keyIdx] ? types[keyIdx]->ToString() : std::string("<null>")));
        }
        out << "      (call radix_sort_indices"
            << " (cast (index values (: " << keyIdx << " i64)) <ptr " << coreType << ">)"
            << " indices work counts n (: " << keyBits << " i64)"
            << " (index descs (: " << keyIdx << " i64)))\n";
    }

    out << "      )))\n";
    return out.str();
}

std::string BuildRadixCompositeNullableWrapperSource(
    const std::vector<NQumir::NAst::TTypePtr>& types)
{
    std::ostringstream out;
    out << "(block\n"
        << "  (fun qdb_radix_sort_indices_composite_nullable\n"
        << "       ((var values <ptr <ptr i8>>)\n"
        << "        (var valids <ptr <ptr u8>>)\n"
        << "        (var indices <ptr u32>)\n"
        << "        (var work <ptr u32>)\n"
        << "        (var counts <ptr u32>)\n"
        << "        (var n i64)\n"
        << "        (var descs <ptr bool>)\n"
        << "        (var nulls_firsts <ptr bool>))\n"
        << "    (block\n";

    for (size_t k = types.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        const auto coreType = SortCoreTypeName(types[keyIdx]);
        const int64_t keyBits = SortRadixKeyBits(types[keyIdx]);
        if (coreType.empty() || keyBits == 0) {
            throw NQumir::TError(
                "CompileRadixSortCompositeNullable: unsupported key type " +
                (types[keyIdx] ? types[keyIdx]->ToString() : std::string("<null>")));
        }
        out << "      (call radix_sort_indices_nullable"
            << " (cast (index values (: " << keyIdx << " i64)) <ptr " << coreType << ">)"
            << " (cast (index valids (: " << keyIdx << " i64)) <ptr u8>)"
            << " indices work counts n (: " << keyBits << " i64)"
            << " (index descs (: " << keyIdx << " i64))"
            << " (index nulls_firsts (: " << keyIdx << " i64)))\n";
    }

    out << "      )))\n";
    return out.str();
}

} // namespace

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

    auto runner = std::make_unique<NQumir::TLLVMRunner>(Opts_);

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "filter", *kernelAst);

    std::string err;
    void* fnPtr = CompileKernelAst(
        *runner,
        std::move(*kernelAst), "<kernel>", &err);
    FinishKernelDiagnostics(Diagnostics_);
    if (!fnPtr) {
        throw std::runtime_error("filter kernel compilation failed: " + err);
    }

    using TFilterFn = void(*)(void*);
    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));

    TFilterDispatch dispatch =
        [fnPtr, sharedRunner, literalStorage](TRowSet& rowSet) {
            reinterpret_cast<TFilterFn>(fnPtr)(&rowSet);
        };

    return dispatch;
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

    auto runner = std::make_unique<NQumir::TLLVMRunner>(Opts_);

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "project", kernelAst);

    std::string err;
    void* fnPtr = CompileKernelAst(*runner, std::move(kernelAst), "<project>", &err);
    FinishKernelDiagnostics(Diagnostics_);
    if (!fnPtr) {
        throw std::runtime_error("project kernel compilation failed: " + err);
    }

    using TProjectFn = void(*)(void*, void**);
    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));
    return [fnPtr, sharedRunner, literalStorage](TRowSet* in, void** outBuffers) {
        reinterpret_cast<TProjectFn>(fnPtr)(in, outBuffers);
    };
}

TKernelCompiler::TSortRadixCompositeDispatch TKernelCompiler::CompileRadixSortComposite(
    const std::vector<NQumir::NAst::TTypePtr>& types)
{
    using namespace NQumir::NAst;

    if (types.empty()) {
        throw NQumir::TError("CompileRadixSortComposite: empty key list");
    }

    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
        if (!library) {
            throw NQumir::TError(
                "CompileRadixSortComposite: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);

    auto wrapper = NKernel::ParseFunctionLibrary(BuildRadixCompositeWrapperSource(types));
    if (!wrapper) {
        throw NQumir::TError(
            "CompileRadixSortComposite: " + wrapper.error().ToString());
    }
    for (auto& stmt : *wrapper) {
        programStmts.push_back(std::move(stmt));
    }

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
    auto runner = std::make_unique<NQumir::TLLVMRunner>(Opts_);

    PrintKernelAst(Diagnostics_, "sort.radix.fused", program);

    std::string err;
    void* fnPtr = CompileKernelAst(
        *runner,
        std::move(program), "qdb_radix_sort_indices_composite", &err);
    FinishKernelDiagnostics(Diagnostics_);
    if (!fnPtr) {
        throw std::runtime_error("sort fused radix kernel compilation failed: " + err);
    }

    using TSortFn = void(*)(void**, uint32_t*, uint32_t*, uint32_t*, int64_t, bool*);
    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));
    return [fnPtr, sharedRunner](void** values, uint32_t* indices, uint32_t* work,
        uint32_t* counts, int64_t n, bool* descs) {
        reinterpret_cast<TSortFn>(fnPtr)(values, indices, work, counts, n, descs);
    };
}

TKernelCompiler::TSortRadixCompositeNullableDispatch
TKernelCompiler::CompileRadixSortCompositeNullable(
    const std::vector<NQumir::NAst::TTypePtr>& types)
{
    using namespace NQumir::NAst;

    if (types.empty()) {
        throw NQumir::TError("CompileRadixSortCompositeNullable: empty key list");
    }

    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
        if (!library) {
            throw NQumir::TError(
                "CompileRadixSortCompositeNullable: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);
    addLibrary("radix_nullable.oz", true);

    auto wrapper = NKernel::ParseFunctionLibrary(
        BuildRadixCompositeNullableWrapperSource(types));
    if (!wrapper) {
        throw NQumir::TError(
            "CompileRadixSortCompositeNullable: " + wrapper.error().ToString());
    }
    for (auto& stmt : *wrapper) {
        programStmts.push_back(std::move(stmt));
    }

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
    auto runner = std::make_unique<NQumir::TLLVMRunner>(Opts_);

    PrintKernelAst(Diagnostics_, "sort.radix.nullable.fused", program);

    std::string err;
    void* fnPtr = CompileKernelAst(
        *runner,
        std::move(program), "qdb_radix_sort_indices_composite_nullable", &err);
    FinishKernelDiagnostics(Diagnostics_);
    if (!fnPtr) {
        throw std::runtime_error("sort fused nullable radix kernel compilation failed: " + err);
    }

    using TSortFn = void(*)(void**, uint8_t**, uint32_t*, uint32_t*, uint32_t*,
        int64_t, bool*, bool*);
    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));
    return [fnPtr, sharedRunner](void** values, uint8_t** valids, uint32_t* indices,
        uint32_t* work, uint32_t* counts, int64_t n, bool* descs, bool* nullsFirsts) {
        reinterpret_cast<TSortFn>(fnPtr)(
            values, valids, indices, work, counts, n, descs, nullsFirsts);
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

    auto dispatchRunner = std::make_shared<NQumir::TLLVMRunner>(Opts_);

    auto dispatchProgram = NKernel::BuildGenericAggregateProgramAst(
        inputType, keyDescriptor, layout,
        columnType, rowSetType, hashTableType);
    if (!dispatchProgram) {
        throw NQumir::TError(
            "CompileAggregate: dispatch program: " + dispatchProgram.error().ToString());
    }
    PrintKernelAst(Diagnostics_, "aggregate.update", *dispatchProgram);
    std::string error;
    void* dispatchFn = CompileKernelAst(
        *dispatchRunner,
        std::move(*dispatchProgram), "agg_dispatch", &error);
    FinishKernelDiagnostics(Diagnostics_);
    if (!dispatchFn) {
        throw std::runtime_error("CompileAggregate: agg_dispatch compilation failed: " + error);
    }

    auto measureRunner = std::make_shared<NQumir::TLLVMRunner>(Opts_);
    auto measureProgram = NKernel::BuildGenericAggregateMeasureProgramAst(
        keyDescriptor, hashTableType);
    if (!measureProgram) {
        throw NQumir::TError(
            "CompileAggregate: measure program: " + measureProgram.error().ToString());
    }
    PrintKernelAst(Diagnostics_, "aggregate.measure", *measureProgram);
    void* measureFn = CompileKernelAst(
        *measureRunner,
        std::move(*measureProgram), "agg_measure_keys", &error);
    FinishKernelDiagnostics(Diagnostics_);
    if (!measureFn) {
        throw std::runtime_error(
            "CompileAggregate: agg_measure_keys compilation failed: " + error);
    }

    auto finalizeRunner = std::make_shared<NQumir::TLLVMRunner>(Opts_);

    auto finalizeProgram = NKernel::BuildGenericAggregateFinalizeProgramAst(
        keyDescriptor, layout, hashTableType, columnType);
    if (!finalizeProgram) {
        throw NQumir::TError(
            "CompileAggregate: finalize program: " + finalizeProgram.error().ToString());
    }
    PrintKernelAst(Diagnostics_, "aggregate.finalize", *finalizeProgram);
    void* finalizeFn = CompileKernelAst(
        *finalizeRunner,
        std::move(*finalizeProgram), "agg_finalize", &error);
    FinishKernelDiagnostics(Diagnostics_);
    if (!finalizeFn) {
        throw std::runtime_error("CompileAggregate: agg_finalize compilation failed: " + error);
    }

    using TDispatchFn = int64_t(*)(void*, TRowSet*, int64_t, int64_t);
    using TMeasureFn = int64_t(*)(void*, int64_t*, int64_t);
    using TFinalizeFn = int64_t(*)(void*, void**, int64_t**, uint8_t**, int64_t);

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
    kernels.Dispatch = [dispatchFn, dispatchRunner](void* ht, TRowSet* batch, int64_t arg, int64_t op) {
        return reinterpret_cast<TDispatchFn>(dispatchFn)(ht, batch, arg, op);
    };
    kernels.Measure = [measureFn, measureRunner](void* ht, int64_t* outputKeyBytes, int64_t outputCapacity) {
        return reinterpret_cast<TMeasureFn>(measureFn)(ht, outputKeyBytes, outputCapacity);
    };
    kernels.Finalize = [finalizeFn, finalizeRunner](void* ht, void** outputKeyBuffers, int64_t** outputBuffers, uint8_t** outputAggMasks, int64_t outputCapacity) {
        return reinterpret_cast<TFinalizeFn>(finalizeFn)(ht, outputKeyBuffers, outputBuffers, outputAggMasks, outputCapacity);
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

    auto compileEntry = [&](const std::string& entry)
        -> std::pair<void*, std::shared_ptr<NQumir::TLLVMRunner>>
    {
        auto options = Opts_;
        options.PrintIr = Diagnostics_ != nullptr;
        options.PrintLlvm = Diagnostics_ != nullptr;
        auto runner = std::make_shared<NQumir::TLLVMRunner>(options);
        auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
        PrintKernelAst(Diagnostics_, "join_hash." + entry, program);
        std::string error;
        void* fn = CompileKernelAst(*runner, program, entry, &error);
        FinishKernelDiagnostics(Diagnostics_);
        if (!fn) {
            throw std::runtime_error(
                "CompileJoinHash: " + entry + " compilation failed: " + error);
        }
        return {fn, std::move(runner)};
    };

    auto [leftFn, leftRunner] = compileEntry("jt_hash_left");
    auto [rightFn, rightRunner] = compileEntry("jt_hash_right");

    using THashFn = bool(*)(TRowSet*, uint64_t*);
    return {
        .Left = [leftFn, leftRunner](TRowSet* batch, uint64_t* hashes) {
            return reinterpret_cast<THashFn>(leftFn)(batch, hashes);
        },
        .Right = [rightFn, rightRunner](TRowSet* batch, uint64_t* hashes) {
            return reinterpret_cast<THashFn>(rightFn)(batch, hashes);
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

    auto compileEntry = [&](const std::string& entry)
        -> std::pair<void*, std::shared_ptr<NQumir::TLLVMRunner>>
    {
        auto options = Opts_;
        options.PrintIr = Diagnostics_ != nullptr;
        options.PrintLlvm = Diagnostics_ != nullptr;
        auto runner = std::make_shared<NQumir::TLLVMRunner>(options);
        auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
        PrintKernelAst(Diagnostics_, "join." + entry, program);
        std::string error;
        void* fn = CompileKernelAst(*runner, program, entry, &error);
        FinishKernelDiagnostics(Diagnostics_);
        if (!fn) {
            throw std::runtime_error("CompileJoin: " + entry + " compilation failed: " + error);
        }
        return {fn, std::move(runner)};
    };

    auto [initFn, initRunner] = compileEntry("jt_init");
    auto [leftFn, leftRunner] = compileEntry("jt_process_left");
    auto [rightFn, rightRunner] = compileEntry("jt_process_right");
    auto [probeLeftFn, probeLeftRunner] = compileEntry("jt_probe_left_stream");
    auto [probeRightFn, probeRightRunner] = compileEntry("jt_probe_right_stream");
    auto [destroyTableFn, destroyTableRunner] = compileEntry("jt_destroy");
    auto [destroyPairsFn, destroyPairsRunner] = compileEntry("pb_destroy");

    using TInitFn = bool(*)(void*, int64_t, int64_t);
    // jt_process_left/right take the two row-store bases (for jt_residual_filter).
    using TProcessFn = bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*);
    using TProbeFn = bool(*)(void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*);
    // jt_insert_key_only keeps the original 5-arg ABI (no residual filter).
    using TInsertKeyOnlyFn = bool(*)(void*, void*, TRowSet*, int64_t, void*);
    using TDestroyFn = void(*)(void*);

    TJoinKernels kernels;
    kernels.Init = [initFn, initRunner, keySize](void* table, int64_t capacity) {
        return reinterpret_cast<TInitFn>(initFn)(table, capacity, keySize);
    };
    kernels.ProcessLeft = [leftFn, leftRunner](void* own, void* opp, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProcessFn>(leftFn)(
            own, opp, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.ProcessRight = [rightFn, rightRunner](void* own, void* opp, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProcessFn>(rightFn)(
            own, opp, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.ProbeLeftStream = [probeLeftFn, probeLeftRunner](void* build, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProbeFn>(probeLeftFn)(
            build, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.ProbeRightStream = [probeRightFn, probeRightRunner](void* build, TRowSet* batch,
        int64_t batchIdx, void* pairs, TRowSet* leftStore, TRowSet* rightStore) {
        return reinterpret_cast<TProbeFn>(probeRightFn)(
            build, batch, batchIdx, pairs, leftStore, rightStore);
    };
    kernels.DestroyTable = [destroyTableFn, destroyTableRunner](void* table) {
        reinterpret_cast<TDestroyFn>(destroyTableFn)(table);
    };
    kernels.DestroyPairs = [destroyPairsFn, destroyPairsRunner](void* pairs) {
        reinterpret_cast<TDestroyFn>(destroyPairsFn)(pairs);
    };

    using TFinalizeFn = bool(*)(void*, void*, void*);

    if (isSemiAnti) {
        auto [insertKeyOnlyFn, insertKeyOnlyRunner] = compileEntry("jt_insert_key_only");
        auto [finalizeFn, finalizeRunner] = compileEntry("jt_finalize_semi_anti");
        kernels.InsertKeyOnly =
            [insertKeyOnlyFn, insertKeyOnlyRunner](void* own, void* opp, TRowSet* batch,
                int64_t batchIdx, void* pairs) {
                return reinterpret_cast<TInsertKeyOnlyFn>(insertKeyOnlyFn)(
                    own, opp, batch, batchIdx, pairs);
            };
        kernels.FinalizeAntiSemi =
            [finalizeFn, finalizeRunner](void* own, void* opp, void* pairs) {
                return reinterpret_cast<TFinalizeFn>(finalizeFn)(own, opp, pairs);
            };
    }

    if (isOuter) {
        auto [finalizeFn, finalizeRunner] = compileEntry("jt_finalize_outer");
        kernels.FinalizeOuter =
            [finalizeFn, finalizeRunner](void* own, void* opp, void* pairs) {
                return reinterpret_cast<TFinalizeFn>(finalizeFn)(own, opp, pairs);
            };
    }

    return kernels;
}

} // namespace NQdb
