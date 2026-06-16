#include <qdb/kernel/compiler.h>
#include <qdb/types/nullable.h>
#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/error.h>
#include <qumir/parser/core/printer.h>

#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace NQqb {

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
    using namespace NQumir::NAst;
    TExprPtr result;
    if (auto ident = TMaybeNode<TIdentExpr>(predicate)) {
        result = std::make_shared<TIdentExpr>(
            predicate->Location, ident.Cast()->Name);
    } else if (auto literal = TMaybeNode<TStringLiteralExpr>(predicate)) {
        result = std::make_shared<TStringLiteralExpr>(
            predicate->Location, literal.Cast()->Value);
    } else if (auto number = TMaybeNode<TNumberExpr>(predicate)) {
        result = number.Cast()->IsFloat()
            ? std::static_pointer_cast<TExpr>(std::make_shared<TNumberExpr>(
                predicate->Location, number.Cast()->FloatValue))
            : std::static_pointer_cast<TExpr>(std::make_shared<TNumberExpr>(
                predicate->Location, number.Cast()->IntValue));
    } else if (auto unary = TMaybeNode<TUnaryExpr>(predicate)) {
        result = std::make_shared<TUnaryExpr>(predicate->Location,
            unary.Cast()->Operator, ClonePredicate(unary.Cast()->Operand));
    } else if (auto binary = TMaybeNode<TBinaryExpr>(predicate)) {
        result = std::make_shared<TBinaryExpr>(predicate->Location,
            binary.Cast()->Operator, ClonePredicate(binary.Cast()->Left),
            ClonePredicate(binary.Cast()->Right));
    } else if (auto call = TMaybeNode<TCallExpr>(predicate)) {
        std::vector<TExprPtr> args;
        args.reserve(call.Cast()->Args.size());
        for (const auto& arg : call.Cast()->Args) {
            args.push_back(ClonePredicate(arg));
        }
        result = std::make_shared<TCallExpr>(predicate->Location,
            ClonePredicate(call.Cast()->Callee), std::move(args));
    } else if (auto cast = TMaybeNode<TCastExpr>(predicate)) {
        result = std::make_shared<TCastExpr>(predicate->Location,
            ClonePredicate(cast.Cast()->Operand), predicate->Type);
    } else {
        throw NQumir::TError(
            "filter predicate clone does not support " +
            std::string(predicate->NodeName()));
    }
    result->Type = predicate->Type;
    return result;
}

} // namespace

TKernelCompiler::TFilterDispatch TKernelCompiler::CompileFilter(
    const NQumir::NAst::TStructType& inputType,
    const NQumir::NAst::TExprPtr& predicate)
{
    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        fieldIndices[inputType.Fields[i].first] = i;
    }

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    const auto& extTypes = dbModule->ExternalTypes();
    // ExternalTypes_[0] = TColumn, [1] = TRowSet (order from qumirdb.cpp)
    NQumir::NAst::TTypePtr columnType;
    NQumir::NAst::TTypePtr rowSetType;
    NQumir::NAst::TTypePtr stringViewType;
    for (const auto& et : extTypes) {
        if (et.Name == "TColumn") {
            columnType = et.Type;
        } else if (et.Name == "TRowSet") {
            rowSetType = et.Type;
        } else if (et.Name == "StringView") {
            stringViewType = et.Type;
        }
    }

    auto literalStorage =
        std::make_shared<std::vector<std::shared_ptr<std::string>>>();
    auto kernelAst = NKernel::BuildFilterProgramAst(
        ClonePredicate(predicate), inputType, fieldIndices, columnType,
        rowSetType, stringViewType, *literalStorage);
    if (!kernelAst) {
        throw NQumir::TError(
            "CompileFilter: " + kernelAst.error().ToString());
    }

    NQumir::TLLVMRunnerOptions opts;
    opts.CoreInput = true;
    opts.ResolveCoreInput = true;
    opts.OptLevel = 3;
    opts.NativeCode = true;
    opts.PrintIr = Diagnostics_ != nullptr;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(opts);
    runner->RegisterModule(dbModule, true);

    PrintKernelAst(Diagnostics_, "filter", *kernelAst);

    std::string err;
    void* fnPtr = runner->CompileKernelAst(
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
    const NQumir::NAst::TStructType& inputType,
    const std::vector<NQumir::NAst::TExprPtr>& computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes)
{
    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        fieldIndices[inputType.Fields[i].first] = i;
    }

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    NQumir::NAst::TTypePtr columnType, rowSetType, stringViewType;
    for (const auto& et : dbModule->ExternalTypes()) {
        if (et.Name == "TColumn") columnType = et.Type;
        else if (et.Name == "TRowSet") rowSetType = et.Type;
        else if (et.Name == "StringView") stringViewType = et.Type;
    }

    auto literalStorage =
        std::make_shared<std::vector<std::shared_ptr<std::string>>>();
    std::vector<NQumir::NAst::TExprPtr> cloned;
    cloned.reserve(computedExprs.size());
    for (const auto& expr : computedExprs) {
        cloned.push_back(ClonePredicate(expr));
    }

    auto kernelAst = NKernel::GenProjectKernelAst(
        std::move(cloned), computedTypes, inputType, fieldIndices,
        columnType, rowSetType, stringViewType, *literalStorage);

    NQumir::TLLVMRunnerOptions opts;
    opts.CoreInput = true;
    opts.ResolveCoreInput = true;
    opts.OptLevel = 3;
    opts.NativeCode = true;
    opts.PrintIr = Diagnostics_ != nullptr;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(opts);
    runner->RegisterModule(dbModule, true);

    PrintKernelAst(Diagnostics_, "project", kernelAst);

    std::string err;
    void* fnPtr = runner->CompileKernelAst(std::move(kernelAst), "<project>", &err);
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

TAggregateKernels TKernelCompiler::CompileAggregate(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs)
{
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

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType, rowSetType, hashTableType;
    for (const auto& et : dbModule->ExternalTypes()) {
        if (et.Name == "TColumn") columnType = et.Type;
        else if (et.Name == "TRowSet") rowSetType = et.Type;
        else if (et.Name == "HashTable") hashTableType = et.Type;
    }

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    options.OptLevel = 3;
    options.PrintIr = Diagnostics_ != nullptr;

    auto dispatchRunner = std::make_shared<NQumir::TLLVMRunner>(options);
    dispatchRunner->RegisterModule(dbModule, true);

    auto dispatchProgram = NKernel::BuildGenericAggregateProgramAst(
        inputType, keyDescriptor, layout,
        columnType, rowSetType, hashTableType);
    if (!dispatchProgram) {
        throw NQumir::TError(
            "CompileAggregate: dispatch program: " + dispatchProgram.error().ToString());
    }
    PrintKernelAst(Diagnostics_, "aggregate.update", *dispatchProgram);
    std::string error;
    void* dispatchFn = dispatchRunner->CompileKernelAst(
        std::move(*dispatchProgram), "agg_dispatch", &error);
    FinishKernelDiagnostics(Diagnostics_);
    if (!dispatchFn) {
        throw std::runtime_error("CompileAggregate: agg_dispatch compilation failed: " + error);
    }

    auto measureRunner = std::make_shared<NQumir::TLLVMRunner>(options);
    measureRunner->RegisterModule(dbModule, true);
    auto measureProgram = NKernel::BuildGenericAggregateMeasureProgramAst(
        keyDescriptor, hashTableType);
    if (!measureProgram) {
        throw NQumir::TError(
            "CompileAggregate: measure program: " + measureProgram.error().ToString());
    }
    PrintKernelAst(Diagnostics_, "aggregate.measure", *measureProgram);
    void* measureFn = measureRunner->CompileKernelAst(
        std::move(*measureProgram), "agg_measure_keys", &error);
    FinishKernelDiagnostics(Diagnostics_);
    if (!measureFn) {
        throw std::runtime_error(
            "CompileAggregate: agg_measure_keys compilation failed: " + error);
    }

    auto finalizeRunner = std::make_shared<NQumir::TLLVMRunner>(options);
    finalizeRunner->RegisterModule(dbModule, true);

    auto finalizeProgram = NKernel::BuildGenericAggregateFinalizeProgramAst(
        keyDescriptor, layout, hashTableType, columnType);
    if (!finalizeProgram) {
        throw NQumir::TError(
            "CompileAggregate: finalize program: " + finalizeProgram.error().ToString());
    }
    PrintKernelAst(Diagnostics_, "aggregate.finalize", *finalizeProgram);
    void* finalizeFn = finalizeRunner->CompileKernelAst(
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
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<std::pair<std::string, std::string>>& keys,
    EJoinType type)
{
    using namespace NQumir::NAst;

    if (type != EJoinType::Inner) {
        throw NQumir::TError("CompileJoin: only INNER join is supported in Stage 1");
    }

    // Unified key descriptor (reuses the aggregation key machinery). Throws on
    // incompatible types / missing columns; string keys are rejected by
    // GenJoinProcessAst below.
    const auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, keys);
    const int64_t keySize = static_cast<int64_t>(keyDesc.Size);

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType, rowSetType, hashTableType, pairBufferType;
    for (const auto& et : dbModule->ExternalTypes()) {
        if (et.Name == "TColumn") columnType = et.Type;
        else if (et.Name == "TRowSet") rowSetType = et.Type;
        else if (et.Name == "HashTable") hashTableType = et.Type;
        else if (et.Name == "PairBuffer") pairBufferType = et.Type;
    }

    // Fresh program per entry (CompileKernelAst consumes the AST): key type
    // decls + key-ops overloads + shared library + generated process functions.
    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        auto library = NKernel::BuildJoinKernelLibrary();
        if (!library) {
            throw NQumir::TError("CompileJoin: " + library.error().ToString());
        }
        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) program.push_back(std::move(f));
        for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) program.push_back(std::move(f));
        for (auto& f : *library) program.push_back(std::move(f));
        program.push_back(NKernel::GenJoinProcessAst(keyDesc, /*isLeft=*/true,
            "jt_process_left", columnType, rowSetType, hashTableType, pairBufferType));
        program.push_back(NKernel::GenJoinProcessAst(keyDesc, /*isLeft=*/false,
            "jt_process_right", columnType, rowSetType, hashTableType, pairBufferType));
        return program;
    };

    auto compileEntry = [&](const std::string& entry)
        -> std::pair<void*, std::shared_ptr<NQumir::TLLVMRunner>>
    {
        NQumir::TLLVMRunnerOptions options;
        options.CoreInput = true;
        options.NativeCode = true;
        options.AllowOverloads = true;
        options.OptLevel = 3;
        options.PrintIr = Diagnostics_ != nullptr;
        auto runner = std::make_shared<NQumir::TLLVMRunner>(options);
        runner->RegisterModule(dbModule, true);
        auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
        PrintKernelAst(Diagnostics_, "join." + entry, program);
        std::string error;
        void* fn = runner->CompileKernelAst(program, entry, &error);
        FinishKernelDiagnostics(Diagnostics_);
        if (!fn) {
            throw std::runtime_error("CompileJoin: " + entry + " compilation failed: " + error);
        }
        return {fn, std::move(runner)};
    };

    auto [initFn, initRunner] = compileEntry("jt_init");
    auto [leftFn, leftRunner] = compileEntry("jt_process_left");
    auto [rightFn, rightRunner] = compileEntry("jt_process_right");
    auto [destroyTableFn, destroyTableRunner] = compileEntry("jt_destroy");
    auto [destroyPairsFn, destroyPairsRunner] = compileEntry("pb_destroy");

    using TInitFn = bool(*)(void*, int64_t, int64_t);
    using TProcessFn = bool(*)(void*, void*, TRowSet*, int64_t, void*);
    using TDestroyFn = void(*)(void*);

    TJoinKernels kernels;
    kernels.Init = [initFn, initRunner, keySize](void* table, int64_t capacity) {
        return reinterpret_cast<TInitFn>(initFn)(table, capacity, keySize);
    };
    kernels.ProcessLeft = [leftFn, leftRunner](void* own, void* opp, TRowSet* batch,
        int64_t batchIdx, void* pairs) {
        return reinterpret_cast<TProcessFn>(leftFn)(own, opp, batch, batchIdx, pairs);
    };
    kernels.ProcessRight = [rightFn, rightRunner](void* own, void* opp, TRowSet* batch,
        int64_t batchIdx, void* pairs) {
        return reinterpret_cast<TProcessFn>(rightFn)(own, opp, batch, batchIdx, pairs);
    };
    kernels.DestroyTable = [destroyTableFn, destroyTableRunner](void* table) {
        reinterpret_cast<TDestroyFn>(destroyTableFn)(table);
    };
    kernels.DestroyPairs = [destroyPairsFn, destroyPairsRunner](void* pairs) {
        reinterpret_cast<TDestroyFn>(destroyPairsFn)(pairs);
    };
    return kernels;
}

} // namespace NQqb
