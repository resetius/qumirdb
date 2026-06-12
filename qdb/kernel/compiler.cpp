#include <qdb/kernel/compiler.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/error.h>
#include <qumir/parser/core/printer.h>

#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace NQqb {

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
    for (const auto& et : extTypes) {
        if (et.Name == "TColumn") {
            columnType = et.Type;
        } else if (et.Name == "TRowSet") {
            rowSetType = et.Type;
        }
    }

    auto kernelAst = NKernel::GenFilterKernelAst(
        predicate, inputType, fieldIndices, columnType, rowSetType);

    NQumir::TLLVMRunnerOptions opts;
    opts.CoreInput = true;
    opts.ResolveCoreInput = true;
    opts.OptLevel = 3;
    opts.NativeCode = true;
    opts.PrintAsm = true;
    opts.PrintIr = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(opts);
    runner->RegisterModule(dbModule, true);

    NQumir::NAst::NCore::PrintAst(std::cerr, kernelAst);

    std::string err;
    void* fnPtr = runner->CompileKernelAst(std::move(kernelAst), &err);
    if (!fnPtr) {
        throw std::runtime_error("filter kernel compilation failed: " + err);
    }

    using TFilterFn = void(*)(void*);
    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));

    TFilterDispatch dispatch = [fnPtr, sharedRunner](TRowSet& rowSet) {
        reinterpret_cast<TFilterFn>(fnPtr)(&rowSet);
    };

    return dispatch;
}

TAggregateKernels TKernelCompiler::CompileAggregate(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs)
{
    using namespace NQumir::NAst;

    if (groupKeys.size() != 1) {
        throw NQumir::TError(
            "CompileAggregate: Stage 1 supports exactly one group key, got " +
            std::to_string(groupKeys.size()));
    }

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
    auto isI64 = [](const TTypePtr& type) {
        auto intType = TMaybeType<TIntegerType>(type);
        return intType && intType.Cast()->Kind == TIntegerType::I64;
    };

    const std::string& keyField = groupKeys[0];
    if (!isI64(requireField(keyField))) {
        throw NQumir::TError("CompileAggregate: group key '" + keyField + "' must be i64 (Stage 1)");
    }

    std::vector<std::string> funcs;
    funcs.reserve(aggs.size());
    std::optional<std::string> argField;
    for (const auto& agg : aggs) {
        if (agg.Func != "count" && agg.Func != "sum" && agg.Func != "min" && agg.Func != "max") {
            throw NQumir::TError("CompileAggregate: unsupported aggregate function '" + agg.Func + "'");
        }
        funcs.push_back(agg.Func);

        if (agg.Arg) {
            auto ident = TMaybeNode<TIdentExpr>(agg.Arg);
            if (!ident) {
                throw NQumir::TError("CompileAggregate: aggregate argument must be a column reference (Stage 1)");
            }
            const std::string& name = ident.Cast()->Name;
            if (argField && *argField != name) {
                throw NQumir::TError(
                    "CompileAggregate: Stage 1 supports a single shared value column for "
                    "all aggregates, got '" + *argField + "' and '" + name + "'");
            }
            argField = name;
        }
    }
    if (argField && !isI64(requireField(*argField))) {
        throw NQumir::TError("CompileAggregate: aggregate argument column '" + *argField + "' must be i64 (Stage 1)");
    }

    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        fieldIndices[inputType.Fields[i].first] = i;
    }

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

    // ---- Dispatch: agg_dispatch (L2c), merged with reduce_0..reduce_{N-1}/
    // agg_apply_reducers (L2a/L2b-1, generated for `funcs`) and count.oz's
    // NumAggs-generic agg_init/agg_rehash/agg_update/agg_destroy (L2b-2),
    // with reducers/agg_apply_reducers preceding agg_update (ordering
    // constraint: type annotation is a single in-order pass).
    auto entryAst = NKernel::GenAggregateKernelAst(
        fieldIndices, keyField, argField, funcs.size(), columnType, rowSetType, hashTableType);
    auto dispatchEntry = TMaybeNode<TBlockExpr>(entryAst).Cast()->Stmts.front();

    auto countLib = NKernel::ParseFunctionLibrary(
        NKernel::ReadAggregationKernel("count.oz"), NKernel::kCountOzFixedFuncs);
    if (!countLib) {
        throw NQumir::TError("count.oz: " + countLib.error().ToString());
    }

    std::vector<TExprPtr> dispatchLib = NKernel::GenReducerFunDecls(funcs);
    dispatchLib.push_back(NKernel::GenApplyReducersFunDecl(funcs.size()));
    dispatchLib.insert(dispatchLib.end(), countLib->begin(), countLib->end());

    auto dispatchRunner = std::make_shared<NQumir::TLLVMRunner>(options);
    dispatchRunner->RegisterModule(dbModule, true);

    auto dispatchMerged = NKernel::MergeKernelLibrary(std::move(dispatchLib), std::move(dispatchEntry));
    std::string error;
    void* dispatchFn = dispatchRunner->CompileKernelAst(std::move(dispatchMerged), &error);
    if (!dispatchFn) {
        throw std::runtime_error("CompileAggregate: agg_dispatch compilation failed: " + error);
    }

    // ---- Finalize: agg_finalize (L2b-2), self-contained (no reducers needed).
    auto finalizeLib = NKernel::ParseFunctionLibrary(
        NKernel::ReadAggregationKernel("finalize.oz"), {"aggregation_finalize"});
    if (!finalizeLib) {
        throw NQumir::TError("finalize.oz: " + finalizeLib.error().ToString());
    }

    auto finalizeFuncs = std::move(*finalizeLib);
    auto finalizeEntry = std::move(finalizeFuncs.back());
    finalizeFuncs.pop_back();

    auto finalizeRunner = std::make_shared<NQumir::TLLVMRunner>(options);
    finalizeRunner->RegisterModule(dbModule, true);

    auto finalizeMerged = NKernel::MergeKernelLibrary(std::move(finalizeFuncs), std::move(finalizeEntry));
    void* finalizeFn = finalizeRunner->CompileKernelAst(std::move(finalizeMerged), &error);
    if (!finalizeFn) {
        throw std::runtime_error("CompileAggregate: agg_finalize compilation failed: " + error);
    }

    using TDispatchFn = int64_t(*)(void*, TRowSet*, int64_t, int64_t);
    using TFinalizeFn = int64_t(*)(void*, int64_t*, int64_t**, int64_t);

    TAggregateKernels kernels;
    kernels.NumAggs = funcs.size();
    kernels.Dispatch = [dispatchFn, dispatchRunner](void* ht, TRowSet* batch, int64_t arg, int64_t op) {
        return reinterpret_cast<TDispatchFn>(dispatchFn)(ht, batch, arg, op);
    };
    kernels.Finalize = [finalizeFn, finalizeRunner](void* ht, int64_t* outputKeys, int64_t** outputBuffers, int64_t outputCapacity) {
        return reinterpret_cast<TFinalizeFn>(finalizeFn)(ht, outputKeys, outputBuffers, outputCapacity);
    };
    return kernels;
}

} // namespace NQqb
