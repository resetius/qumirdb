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
    } else if (auto ifExpr = TMaybeNode<TIfExpr>(predicate)) {
        result = std::make_shared<TIfExpr>(predicate->Location,
            ClonePredicate(ifExpr.Cast()->Cond),
            ClonePredicate(ifExpr.Cast()->Then),
            ifExpr.Cast()->Else ? ClonePredicate(ifExpr.Cast()->Else) : nullptr);
    } else if (auto block = TMaybeNode<TBlockExpr>(predicate)) {
        std::vector<TExprPtr> stmts;
        stmts.reserve(block.Cast()->Stmts.size());
        for (const auto& s : block.Cast()->Stmts) stmts.push_back(ClonePredicate(s));
        result = std::make_shared<TBlockExpr>(predicate->Location, std::move(stmts));
    } else if (auto var = TMaybeNode<TVarStmt>(predicate)) {
        std::vector<std::pair<TExprPtr, TExprPtr>> bounds;
        bounds.reserve(var.Cast()->Bounds.size());
        for (const auto& [lo, hi] : var.Cast()->Bounds)
            bounds.emplace_back(ClonePredicate(lo), ClonePredicate(hi));
        auto v = std::make_shared<TVarStmt>(predicate->Location,
            var.Cast()->Name, predicate->Type, std::move(bounds));
        if (var.Cast()->Init) v->Init = ClonePredicate(var.Cast()->Init);
        result = std::move(v);
    } else if (auto assign = TMaybeNode<TAssignExpr>(predicate)) {
        result = std::make_shared<TAssignExpr>(predicate->Location,
            assign.Cast()->Name, ClonePredicate(assign.Cast()->Value));
    } else if (auto aarr = TMaybeNode<TArrayAssignExpr>(predicate)) {
        std::vector<TExprPtr> idxs;
        idxs.reserve(aarr.Cast()->Indices.size());
        for (const auto& i : aarr.Cast()->Indices) idxs.push_back(ClonePredicate(i));
        result = std::make_shared<TArrayAssignExpr>(predicate->Location,
            aarr.Cast()->Name, std::move(idxs), ClonePredicate(aarr.Cast()->Value));
    } else if (auto ret = TMaybeNode<TReturnExpr>(predicate)) {
        result = std::make_shared<TReturnExpr>(predicate->Location,
            ret.Cast()->Value ? ClonePredicate(ret.Cast()->Value) : nullptr);
    } else if (TMaybeNode<TBreakStmt>(predicate)) {
        result = std::make_shared<TBreakStmt>(predicate->Location);
    } else if (TMaybeNode<TContinueStmt>(predicate)) {
        result = std::make_shared<TContinueStmt>(predicate->Location);
    } else if (auto idx = TMaybeNode<TIndexExpr>(predicate)) {
        result = std::make_shared<TIndexExpr>(predicate->Location,
            ClonePredicate(idx.Cast()->Collection), ClonePredicate(idx.Cast()->Index));
    } else if (auto midx = TMaybeNode<TMultiIndexExpr>(predicate)) {
        std::vector<TExprPtr> indices;
        indices.reserve(midx.Cast()->Indices.size());
        for (const auto& i : midx.Cast()->Indices) indices.push_back(ClonePredicate(i));
        result = std::make_shared<TMultiIndexExpr>(predicate->Location,
            ClonePredicate(midx.Cast()->Collection), std::move(indices));
    } else if (auto slice = TMaybeNode<TSliceExpr>(predicate)) {
        result = std::make_shared<TSliceExpr>(predicate->Location,
            ClonePredicate(slice.Cast()->Collection),
            ClonePredicate(slice.Cast()->Start),
            ClonePredicate(slice.Cast()->End));
    } else if (auto fa = TMaybeNode<TFieldAccessExpr>(predicate)) {
        result = std::make_shared<TFieldAccessExpr>(predicate->Location,
            ClonePredicate(fa.Cast()->Object), fa.Cast()->FieldName);
    } else if (auto fassign = TMaybeNode<TFieldAssignExpr>(predicate)) {
        result = std::make_shared<TFieldAssignExpr>(predicate->Location,
            ClonePredicate(fassign.Cast()->Object), fassign.Cast()->FieldName,
            ClonePredicate(fassign.Cast()->Value));
    } else if (auto sc = TMaybeNode<TStructConstructExpr>(predicate)) {
        std::vector<TExprPtr> fields;
        fields.reserve(sc.Cast()->Fields.size());
        for (const auto& f : sc.Cast()->Fields) fields.push_back(ClonePredicate(f));
        result = std::make_shared<TStructConstructExpr>(predicate->Location,
            predicate->Type, std::move(fields));
    } else if (auto wh = TMaybeNode<TWhileStmtExpr>(predicate)) {
        result = std::make_shared<TWhileStmtExpr>(predicate->Location,
            ClonePredicate(wh.Cast()->Cond), ClonePredicate(wh.Cast()->Body));
    } else if (auto rep = TMaybeNode<TRepeatStmtExpr>(predicate)) {
        result = std::make_shared<TRepeatStmtExpr>(predicate->Location,
            ClonePredicate(rep.Cast()->Body), ClonePredicate(rep.Cast()->Cond));
    } else if (auto forst = TMaybeNode<TForStmtExpr>(predicate)) {
        result = std::make_shared<TForStmtExpr>(predicate->Location,
            forst.Cast()->VarName,
            ClonePredicate(forst.Cast()->From), ClonePredicate(forst.Cast()->To),
            ClonePredicate(forst.Cast()->Step), ClonePredicate(forst.Cast()->Body));
    } else if (auto times = TMaybeNode<TTimesStmtExpr>(predicate)) {
        result = std::make_shared<TTimesStmtExpr>(predicate->Location,
            ClonePredicate(times.Cast()->Count), ClonePredicate(times.Cast()->Body));
    } else if (auto await = TMaybeNode<TAwaitExpr>(predicate)) {
        result = std::make_shared<TAwaitExpr>(predicate->Location,
            ClonePredicate(await.Cast()->Operand));
    } else if (auto asrt = TMaybeNode<TAssertStmt>(predicate)) {
        result = std::make_shared<TAssertStmt>(predicate->Location,
            ClonePredicate(asrt.Cast()->Expr));
    } else if (TMaybeNode<TTypeDeclStmt>(predicate)) {
        result = std::make_shared<TTypeDeclStmt>(predicate->Location, predicate->Type);
    } else {
        throw NQumir::TError(
            "CloneExpr: unsupported node type " +
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

    auto runner = std::make_unique<NQumir::TLLVMRunner>(Opts_);
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
    std::vector<NQumir::NAst::TExprPtr> cloned;
    cloned.reserve(computedExprs.size());
    for (const auto& expr : computedExprs) {
        cloned.push_back(ClonePredicate(expr));
    }

    auto kernelAst = NKernel::GenProjectKernelAst(
        std::move(cloned), computedTypes, inputType, fieldIndices,
        columnType, rowSetType, stringViewType, *literalStorage);

    auto runner = std::make_unique<NQumir::TLLVMRunner>(Opts_);
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
        if (et.Name == "TColumn") {
            columnType = et.Type;
        } else if (et.Name == "TRowSet") {
            rowSetType = et.Type;
        } else if (et.Name == "HashTable") {
            hashTableType = et.Type;
        }
    }

    auto dispatchRunner = std::make_shared<NQumir::TLLVMRunner>(Opts_);
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

    auto measureRunner = std::make_shared<NQumir::TLLVMRunner>(Opts_);
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

    auto finalizeRunner = std::make_shared<NQumir::TLLVMRunner>(Opts_);
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

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType, rowSetType, hashTableType, pairBufferType, stringViewType;
    for (const auto& et : dbModule->ExternalTypes()) {
        if (et.Name == "TColumn") {
            columnType = et.Type;
        } else if (et.Name == "TRowSet") {
            rowSetType = et.Type;
        } else if (et.Name == "HashTable") {
            hashTableType = et.Type;
        } else if (et.Name == "PairBuffer") {
            pairBufferType = et.Type;
        } else if (et.Name == "StringView") {
            stringViewType = et.Type;
        }
    }

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
        NQumir::TLLVMRunnerOptions options;
        options.CoreInput = true;
        options.NativeCode = true;
        options.AllowOverloads = true;
        options.OptLevel = 3;
        options.PrintIr = Diagnostics_ != nullptr;
        options.PrintLlvm = Diagnostics_ != nullptr;
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
    // jt_process_left/right take the two row-store bases (for jt_residual_filter).
    using TProcessFn = bool(*)(void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*);
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

} // namespace NQqb
