#include <qdb/kernel/compiler.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/column_value.h>
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

NQumir::NAst::TTypePtr SortCoreType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return std::make_shared<TIntegerType>(integer.Cast()->Kind);
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return std::make_shared<TFloatType>();
    }
    return nullptr;
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

NQumir::NAst::TExprPtr Int64Literal(int64_t value) {
    auto expr = NKernel::NOz::Int(value);
    expr->Type = std::make_shared<NQumir::NAst::TIntegerType>();
    return expr;
}

NQumir::NAst::TTypePtr Ptr(NQumir::NAst::TTypePtr type) {
    return std::make_shared<NQumir::NAst::TPointerType>(std::move(type));
}

NQumir::NAst::TExprPtr Cast(
    NQumir::NAst::TExprPtr expr,
    NQumir::NAst::TTypePtr type)
{
    return std::make_shared<NQumir::NAst::TCastExpr>(
        NQumir::TLocation{}, std::move(expr), std::move(type));
}

NQumir::NAst::TExprPtr BuildRowIdSortCall(
    const TSortRadixKeyInput& key,
    bool nullable,
    NQumir::NAst::TExprPtr desc,
    NQumir::NAst::TExprPtr nullsFirst,
    std::string storeName)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto columnIndex = Int64Literal(key.ColumnIndex);
    auto ptrU64 = Ptr(std::make_shared<TIntegerType>(TIntegerType::U64));

    if (SortKeyIsString(key.Type)) {
        std::vector<TExprPtr> args{
            Oz::Ident(storeName),
            std::move(columnIndex),
            Oz::Ident("row_ids"),
            Cast(Oz::Ident("work"), ptrU64),
            Oz::Ident("counts"),
            Oz::Ident("n"),
            std::move(desc),
        };
        if (nullable) {
            args.push_back(std::move(nullsFirst));
        }
        return Oz::Call(
            nullable ? "sort_string_rowids_nullable" : "sort_string_rowids",
            std::move(args));
    }
    if (SortKeyIsBool(key.Type)) {
        std::vector<TExprPtr> args{
            Oz::Ident(storeName),
            std::move(columnIndex),
            Oz::Ident("row_ids"),
            Oz::Ident("work"),
            Oz::Ident("counts"),
            Oz::Ident("n"),
            std::move(desc),
        };
        if (nullable) {
            args.push_back(std::move(nullsFirst));
        }
        return Oz::Call(
            nullable ? "sort_bool_rowids_nullable" : "sort_bool_rowids",
            std::move(args));
    }

    auto coreType = SortCoreType(key.Type);
    const int64_t keyBits = SortRadixKeyBits(key.Type);
    if (!coreType || keyBits == 0) {
        throw NQumir::TError(
            "CompileRadixSortComposite: unsupported key type " +
            (key.Type ? key.Type->ToString() : std::string("<null>")));
    }
    std::vector<TExprPtr> args{
        Oz::Ident(storeName),
        std::move(columnIndex),
        Oz::Ident("row_ids"),
        Oz::Ident("work"),
        Oz::Ident("counts"),
        Oz::Ident("n"),
        Int64Literal(keyBits),
        std::move(desc),
    };
    if (nullable) {
        args.push_back(std::move(nullsFirst));
    }
    args.push_back(Cast(Int64Literal(0), Ptr(std::move(coreType))));
    return Oz::Call(
        nullable ? "sort_fixed_rowids_nullable" : "sort_fixed_rowids",
        std::move(args));
}

NQumir::NAst::TExprPtr BuildDynamicRowIdSortCall(
    const TSortRadixKeyInput& key,
    size_t keyIdx,
    bool nullable)
{
    namespace Oz = NKernel::NOz;
    auto desc = Oz::Index("descs", Int64Literal(static_cast<int64_t>(keyIdx)));
    auto nullsFirst = Oz::Index(
        "nulls_firsts", Int64Literal(static_cast<int64_t>(keyIdx)));
    return BuildRowIdSortCall(
        key, nullable, std::move(desc), std::move(nullsFirst), "store");
}

NQumir::NAst::TExprPtr BuildRadixCompositeWrapperAst(
    const std::vector<TSortRadixKeyInput>& keys,
    bool nullable)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto u32Type = std::make_shared<TIntegerType>(TIntegerType::U32);
    auto boolType = std::make_shared<TBoolType>();
    auto rowSetPtrType = Ptr(QumirDbNamedType("TRowSet"));
    auto ptrI64Type = Ptr(i64Type);
    auto ptrU32Type = Ptr(u32Type);
    auto ptrBoolType = Ptr(boolType);

    Oz::TFunBuilder builder(
        nullable
            ? "qdb_radix_sort_indices_composite_nullable"
            : "qdb_radix_sort_indices_composite");
    builder
        .Param("store", rowSetPtrType)
        .Param("row_ids", ptrI64Type)
        .Param("work", ptrI64Type)
        .Param("counts", ptrU32Type)
        .Param("n", i64Type)
        .Param("descs", ptrBoolType)
        .Return(std::make_shared<TVoidType>());
    if (nullable) {
        builder.Param("nulls_firsts", ptrBoolType);
    }

    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        builder.Stmt(BuildDynamicRowIdSortCall(keys[keyIdx], keyIdx, nullable));
    }
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr Field(
    NQumir::NAst::TExprPtr object,
    std::string field)
{
    return std::make_shared<NQumir::NAst::TFieldAccessExpr>(
        NQumir::TLocation{}, std::move(object), std::move(field));
}

NQumir::NAst::TExprPtr Unary(
    std::string op,
    NQumir::NAst::TExprPtr value)
{
    return std::make_shared<NQumir::NAst::TUnaryExpr>(
        NQumir::TLocation{}, NQumir::NAst::TOperator(op), std::move(value));
}

NQumir::NAst::TExprPtr ArrayAssign(
    std::string name,
    NQumir::NAst::TExprPtr index,
    NQumir::NAst::TExprPtr value)
{
    std::vector<NQumir::NAst::TExprPtr> indices;
    indices.push_back(std::move(index));
    return std::make_shared<NQumir::NAst::TArrayAssignExpr>(
        NQumir::TLocation{}, std::move(name), std::move(indices), std::move(value));
}

NQumir::NAst::TExprPtr TypedInt(
    int64_t value,
    NQumir::NAst::TTypePtr type)
{
    auto expr = std::make_shared<NQumir::NAst::TNumberExpr>(
        NQumir::TLocation{}, value);
    expr->Type = std::move(type);
    return expr;
}

NQumir::NAst::TExprPtr BoolConst(bool value) {
    return NKernel::NOz::Bool(value);
}

NQumir::NAst::TExprPtr Deref(NQumir::NAst::TExprPtr ptr) {
    return NKernel::NOz::Index(std::move(ptr), Int64Literal(0));
}

NQumir::NAst::TExprPtr Low32(NQumir::NAst::TExprPtr value) {
    return NKernel::NOz::Bin(
        NQumir::NAst::TOperator("&"),
        std::move(value),
        Int64Literal(4294967295LL));
}

void AppendSetup(
    NKernel::NOz::TFunBuilder& builder,
    std::vector<NQumir::NAst::TExprPtr>& setup)
{
    for (auto& stmt : setup) {
        builder.Stmt(std::move(stmt));
    }
}

NQumir::NAst::TExprPtr ComparableValue(
    const NKernel::TColumnValueAst& value,
    const NQumir::NAst::TTypePtr& i64Type)
{
    using namespace NQumir::NAst;
    if (TMaybeType<TBoolType>(UnwrapNamedType(value.ValueType))) {
        return Cast(value.Value, i64Type);
    }
    return value.Value;
}

NQumir::NAst::TExprPtr BuildTopSortTempBeforeStateAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto rowSetPtrType = Ptr(rowSetType);
    auto columnType = QumirDbNamedType("TColumn");
    auto columnPtrType = Ptr(columnType);
    auto stringViewType = QumirDbNamedType("StringView");

    Oz::TFunBuilder builder("qdb_top_sort_temp_before_state");
    builder
        .Param("state", rowSetPtrType)
        .Param("batch", rowSetPtrType)
        .Param("state_row", i64Type)
        .Param("temp_row", i64Type)
        .Return(std::make_shared<TBoolType>())
        .Var("state_rs", rowSetType)
        .Assign("state_rs", Deref(Oz::Ident("state")))
        .Var("batch_rs", rowSetType)
        .Assign("batch_rs", Deref(Oz::Ident("batch")))
        .Var("state_cols", columnPtrType)
        .Assign("state_cols", Field(Oz::Ident("state_rs"), "Columns"))
        .Var("batch_cols", columnPtrType)
        .Assign("batch_cols", Field(Oz::Ident("batch_rs"), "Columns"));

    for (size_t k = 0; k < keys.size(); ++k) {
        const auto& key = keys[k];
        const std::string suffix = std::to_string(k);
        const std::string stateCol = "state_col_" + suffix;
        const std::string tempCol = "temp_col_" + suffix;

        builder
            .Var(stateCol, columnType)
            .Assign(stateCol,
                Oz::Index("state_cols", Int64Literal(key.ColumnIndex)))
            .Var(tempCol, columnType)
            .Assign(tempCol,
                Oz::Index("batch_cols", Int64Literal(key.ColumnIndex)));

        auto stateValue = NKernel::BuildColumnValueAst(
            stateCol, "state_row", "state_key_" + suffix,
            key.Type, stringViewType);
        auto tempValue = NKernel::BuildColumnValueAst(
            tempCol, "temp_row", "temp_key_" + suffix,
            key.Type, stringViewType);
        AppendSetup(builder, stateValue.Setup);
        AppendSetup(builder, tempValue.Setup);

        auto anyInvalid = Oz::Bin(TOperator("||"),
            Unary("!", stateValue.IsValid),
            Unary("!", tempValue.IsValid));
        auto validityDiffers = Oz::Bin(TOperator("!="),
            stateValue.IsValid, tempValue.IsValid);
        auto tempBeforeByNull = key.NullsFirst
            ? Unary("!", tempValue.IsValid)
            : tempValue.IsValid;
        builder.Stmt(Oz::If(
            std::move(anyInvalid),
            Oz::Block({
                Oz::If(std::move(validityDiffers),
                    Oz::Block({Oz::Return(std::move(tempBeforeByNull))}))
            })));

        auto tempComparable = ComparableValue(tempValue, i64Type);
        auto stateComparable = ComparableValue(stateValue, i64Type);
        builder.Stmt(Oz::If(
            Oz::Bin(TOperator("<"), tempComparable, stateComparable),
            Oz::Block({Oz::Return(BoolConst(!key.Desc))})));
        builder.Stmt(Oz::If(
            Oz::Bin(TOperator("<"), stateComparable, tempComparable),
            Oz::Block({Oz::Return(BoolConst(key.Desc))})));
    }

    builder.Stmt(Oz::Return(BoolConst(false)));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildTopSortMergePicksAst()
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u32Type = std::make_shared<TIntegerType>(TIntegerType::U32);
    auto rowSetPtrType = Ptr(QumirDbNamedType("TRowSet"));
    auto ptrI64Type = Ptr(i64Type);
    auto ptrU8Type = Ptr(u8Type);
    auto ptrU32Type = Ptr(u32Type);

    auto makePickState = [&]() {
        return Oz::Block({
            ArrayAssign("pick_src", Oz::Ident("out"), TypedInt(0, u8Type)),
            ArrayAssign("pick_idx", Oz::Ident("out"),
                Cast(Oz::Ident("left"), u32Type)),
            Oz::Assign("left", Oz::Add(Oz::Ident("left"), Int64Literal(1))),
        });
    };
    auto makePickTemp = [&]() {
        return Oz::Block({
            Oz::Assign("temp_id", Oz::Index("temp_row_ids", Oz::Ident("right"))),
            Oz::Assign("temp_row", Low32(Oz::Ident("temp_id"))),
            ArrayAssign("pick_src", Oz::Ident("out"), TypedInt(1, u8Type)),
            ArrayAssign("pick_idx", Oz::Ident("out"),
                Cast(Oz::Ident("temp_row"), u32Type)),
            Oz::Assign("right", Oz::Add(Oz::Ident("right"), Int64Literal(1))),
        });
    };

    Oz::TFunBuilder builder("qdb_top_sort_merge_picks");
    builder
        .Param("state", rowSetPtrType)
        .Param("batch", rowSetPtrType)
        .Param("temp_row_ids", ptrI64Type)
        .Param("temp_n", i64Type)
        .Param("pick_src", ptrU8Type)
        .Param("pick_idx", ptrU32Type)
        .Param("limit", i64Type)
        .Return(i64Type)
        .Var("state_rs", QumirDbNamedType("TRowSet"))
        .Assign("state_rs", Deref(Oz::Ident("state")))
        .Var("state_n", i64Type)
        .Assign("state_n", Field(Oz::Ident("state_rs"), "RowCount"))
        .Var("left", i64Type)
        .Assign("left", Int64Literal(0))
        .Var("right", i64Type)
        .Assign("right", Int64Literal(0))
        .Var("out", i64Type)
        .Assign("out", Int64Literal(0))
        .Var("temp_id", i64Type)
        .Assign("temp_id", Int64Literal(0))
        .Var("temp_row", i64Type)
        .Assign("temp_row", Int64Literal(0));

    auto hasOutputRoom = Oz::Bin(TOperator("<"), Oz::Ident("out"), Oz::Ident("limit"));
    auto hasState = Oz::Bin(TOperator("<"), Oz::Ident("left"), Oz::Ident("state_n"));
    auto hasTemp = Oz::Bin(TOperator("<"), Oz::Ident("right"), Oz::Ident("temp_n"));
    auto loopCond = Oz::Bin(TOperator("&&"), std::move(hasOutputRoom),
        Oz::Bin(TOperator("||"), std::move(hasState), std::move(hasTemp)));
    auto tempBeforeState = Oz::Call("qdb_top_sort_temp_before_state", {
        Oz::Ident("state"),
        Oz::Ident("batch"),
        Oz::Ident("left"),
        Oz::Ident("temp_row"),
    });
    auto chooseByCompare = Oz::Block({
        Oz::Assign("temp_id", Oz::Index("temp_row_ids", Oz::Ident("right"))),
        Oz::Assign("temp_row", Low32(Oz::Ident("temp_id"))),
        Oz::If(std::move(tempBeforeState), makePickTemp(), makePickState()),
    });
    auto loopBody = Oz::Block({
        Oz::If(
            Oz::Bin(TOperator("=="), Oz::Ident("right"), Oz::Ident("temp_n")),
            makePickState(),
            Oz::If(
                Oz::Bin(TOperator("=="), Oz::Ident("left"), Oz::Ident("state_n")),
                makePickTemp(),
                std::move(chooseByCompare))),
        Oz::Assign("out", Oz::Add(Oz::Ident("out"), Int64Literal(1))),
    });
    builder.Stmt(Oz::While(std::move(loopCond), std::move(loopBody)));
    builder.Stmt(Oz::Return(Oz::Ident("out")));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildTopSortUpdateAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u32Type = std::make_shared<TIntegerType>(TIntegerType::U32);
    auto rowSetPtrType = Ptr(QumirDbNamedType("TRowSet"));
    auto ptrI64Type = Ptr(i64Type);
    auto ptrU32Type = Ptr(u32Type);
    auto ptrU8Type = Ptr(u8Type);

    bool anyNullableKey = false;
    for (const auto& key : keys) {
        anyNullableKey = anyNullableKey || IsNullableType(key.Type);
    }

    Oz::TFunBuilder builder("qdb_top_sort_update");
    builder
        .Param("state", rowSetPtrType)
        .Param("batch", rowSetPtrType)
        .Param("row_ids", ptrI64Type)
        .Param("work", ptrI64Type)
        .Param("counts", ptrU32Type)
        .Param("n", i64Type)
        .Param("pick_src", ptrU8Type)
        .Param("pick_idx", ptrU32Type)
        .Param("limit", i64Type)
        .Return(i64Type);

    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        builder.Stmt(BuildRowIdSortCall(
            keys[keyIdx],
            anyNullableKey,
            BoolConst(keys[keyIdx].Desc),
            BoolConst(keys[keyIdx].NullsFirst),
            "batch"));
    }
    builder.Stmt(Oz::Return(Oz::Call("qdb_top_sort_merge_picks", {
        Oz::Ident("state"),
        Oz::Ident("batch"),
        Oz::Ident("row_ids"),
        Oz::Ident("n"),
        Oz::Ident("pick_src"),
        Oz::Ident("pick_idx"),
        Oz::Ident("limit"),
    })));
    return std::move(builder).Build();
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
    programStmts.push_back(BuildRadixCompositeWrapperAst(keys, false));
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
    programStmts.push_back(BuildRadixCompositeWrapperAst(keys, true));
    return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildTopSortMergeProgramAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("BuildTopSortMergeProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
        if (!library) {
            throw NQumir::TError(
                "BuildTopSortMergeProgramAst: " + library.error().ToString());
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
    programStmts.push_back(BuildTopSortTempBeforeStateAst(keys));
    programStmts.push_back(BuildTopSortMergePicksAst());
    programStmts.push_back(BuildTopSortUpdateAst(keys));
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

TKernelCompiler::TTopSortDispatch TKernelCompiler::CompileTopSort(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;

    auto program = BuildTopSortMergeProgramAst(keys);
    PrintKernelAst(Diagnostics_, "top-sort.fused", program);

    auto kernel = EmitKernel(
        "top-sort.fused",
        {"qdb_top_sort_update"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using TTopSortFn = int64_t(*)(
        TRowSet*, TRowSet*, int64_t*, int64_t*, uint32_t*,
        int64_t, uint8_t*, uint32_t*, int64_t);
    return [slot = kernel.Slot](
        TRowSet* state,
        TRowSet* batch,
        int64_t* rowIds,
        int64_t* work,
        uint32_t* counts,
        int64_t n,
        uint8_t* pickSrc,
        uint32_t* pickIdx,
        int64_t limit) {
        return reinterpret_cast<TTopSortFn>(slot->Fns[0])(
            state, batch, rowIds, work, counts, n, pickSrc, pickIdx, limit);
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
        {"agg_dispatch", "agg_finish_rowset"},
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
    using TFinishRowSetFn = int64_t(*)(void*, TRowSet*);

    auto slot = kernel.Slot;
    kernels.Dispatch = [slot](void* ht, TRowSet* batch, int64_t arg, int64_t op) {
        return reinterpret_cast<TDispatchFn>(slot->Fns[0])(ht, batch, arg, op);
    };
    kernels.FinishRowSet = [slot](void* ht, TRowSet* output) {
        return reinterpret_cast<TFinishRowSetFn>(slot->Fns[1])(ht, output);
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

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    return CompileJoin(
        *leftType.Cast(), *rightType.Cast(), keys, spec.JoinType,
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
    const bool isResidualSemiAnti = isSemiAnti && static_cast<bool>(residualPredicate);
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
        if (isSemiAnti && !isResidualSemiAnti) {
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
        program.push_back(NKernel::GenJoinDispatchAst(
            keySize, type, isResidualSemiAnti,
            rowSetType, hashTableType, pairBufferType));
        return program;
    };

    std::vector<std::string> entries = {"jt_dispatch"};

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "join", program);

    auto kernel = EmitKernel("join", entries, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto slot = kernel.Slot;
    using TDispatchFn = bool(*)(
        void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*, int64_t, int64_t);

    TJoinKernels kernels;
    kernels.Dispatch = [slot](
        void* left,
        void* right,
        TRowSet* batch,
        int64_t batchIdx,
        void* pairs,
        TRowSet* leftStore,
        TRowSet* rightStore,
        int64_t arg,
        int64_t op) {
        return reinterpret_cast<TDispatchFn>(slot->Fns[0])(
            left, right, batch, batchIdx, pairs, leftStore, rightStore, arg, op);
    };
    return kernels;
}

} // namespace NQdb
