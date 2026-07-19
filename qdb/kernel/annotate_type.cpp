#include <qdb/kernel/annotate_type.h>

#include <qdb/plan/clone_expr.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>
#include <qumir/parser/core/printer.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/modules/system/system.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NQdb::NKernel {

using namespace NQumir::NAst;

namespace {

// TError carries its text in Location/Children, not Msg, so what() is empty for the
// generic std::exception catch. Re-throw with the full ToString() flattened into Msg
// so both what() and ToString() are informative wherever the error is caught.
[[noreturn]] void ThrowTyping(std::string_view stage, const NQumir::TError& cause) {
    throw NQumir::TError(
        "project type inference [" + std::string(stage) + "]: " + cause.ToString());
}

// Planner column type -> the qumir type the annotator should see. String columns are
// StringView in kernels; nullable columns use qumirdb.oz's Nullable[T] alias so
// operator result types come from the same source-module overloads kernels use.
TTypePtr ToQumirType(const TTypePtr& type) {
    if (!type) {
        return type;
    }
    if (IsNullableType(type)) {
        return std::make_shared<TNamedType>(
            "Nullable",
            nullptr,
            std::vector<TGenericArg>{
                TGenericArg::TypeArg(ToQumirType(UnwrapNullableType(type)))});
    }
    if (TMaybeType<TStringType>(type)) {
        return std::make_shared<TNamedType>("StringView", nullptr);
    }
    return type;
}

// Annotator result -> planner type. Map qumirdb.oz source-module aliases back to
// planner types.
TTypePtr FromQumirType(const TTypePtr& type) {
    TTypePtr t = type;
    if (auto named = TMaybeType<TNamedType>(t);
        named && named.Cast()->Name == "Nullable" && !named.Cast()->TypeArgs.empty())
    {
        return std::make_shared<TNullable>(
            FromQumirType(named.Cast()->TypeArgs.front().Type));
    }
    if (auto named = TMaybeType<TNamedType>(t); named && named.Cast()->Name == "StringView") {
        return std::make_shared<TStringType>();
    }
    // DATE/TIMESTAMP are erased to i32 at the schema boundary (parquet DATE32 -> i32);
    // a `cast(... as date)` target carries no underlying type, so map it explicitly.
    if (auto named = TMaybeType<TNamedType>(t);
        named && (named.Cast()->Name == "DATE" || named.Cast()->Name == "TIMESTAMP"))
    {
        return std::make_shared<TIntegerType>(TIntegerType::I32);
    }
    return UnwrapNamedType(UnwrapNullableType(t));
}

// Finds the __result__ var in the annotated AST and returns its inferred type. The
// pipeline may rewrite/replace nodes, so we locate it by name rather than keeping a
// pointer to the pre-annotation node.
TTypePtr FindResultType(const TExprPtr& node, const std::string& resultName) {
    if (!node) {
        return nullptr;
    }
    if (auto var = TMaybeNode<TVarStmt>(node); var && var.Cast()->Name == resultName) {
        if (var.Cast()->Type) {
            return var.Cast()->Type;
        }
        if (var.Cast()->Init && var.Cast()->Init->Type) {
            return var.Cast()->Init->Type;
        }
    }
    for (auto* child : node->MutableChildren()) {
        if (auto found = FindResultType(*child, resultName)) {
            return found;
        }
    }
    return nullptr;
}

// Runs the real qumir annotator on a throwaway function whose params are the input
// columns, so external-function and operator return types come from qumirdb.oz.
class TTypingContext {
public:
    TTypingContext() {
        System_ = std::make_shared<NQumir::NRegistry::SystemModule>();
        Resolver_.ApplyPragmas(Pragmas_);
        Resolver_.RegisterModule(System_.get());
        if (auto reg = Loader_.RegisterSourceModule(
                std::string(QDB_SOURCE_DIR) + "/modules/qumirdb.oz"); !reg) {
            LoadError_ = reg.error();
        }
    }

    // Planner return type of an extern function declared in qumirdb.oz (e.g. qdb_substring
    // -> string), or nullptr if unknown. Built once from the resolver's external-function
    // set — a single qumirdb.oz compose, not a per-node one.
    TTypePtr ExternReturnType(const std::string& name) {
        EnsureExterns();
        auto it = ExternReturns_.find(name);
        return it == ExternReturns_.end() ? nullptr : it->second;
    }

    TTypePtr Annotate(const TExprPtr& expr, const TStructType& inputType) {
        if (LoadError_) {
            ThrowTyping("load qumirdb.oz", *LoadError_);
        }
        NQumir::TLocation loc{};

        std::vector<TParam> params;
        params.reserve(inputType.Fields.size());
        for (const auto& [name, type] : inputType.Fields) {
            params.push_back(std::make_shared<TVarStmt>(loc, name, ToQumirType(type)));
        }

        // Annotation writes ->Type in place, so type a clone, not the plan's node.
        auto cloned = CloneExpr(expr);
        // The resolver is shared, so each throwaway function needs a unique name — a
        // fixed one would clash ("overload differs only in return type") on reuse.
        // TODO: reclaim the per-call symbol/scope instead of leaking a name (needs a
        // name_resolver checkpoint/rollback that survives generic instantiation).
        const std::string tag = std::to_string(Counter_++);
        const std::string resultName = "__result_" + tag + "__";
        auto resultVar = std::make_shared<TVarStmt>(loc, resultName, nullptr);
        resultVar->Init = cloned;
        auto body = std::make_shared<TBlockExpr>(
            loc, std::vector<TExprPtr>{resultVar});
        auto fn = std::make_shared<TFunDecl>(
            loc, "__annotate_" + tag + "__", std::vector<TGenericParam>{},
            std::move(params), body, std::make_shared<TVoidType>());
        TExprPtr module = std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
            std::make_shared<TUseExpr>(loc, "qumirdb"), fn});

        // Shared loader + resolver so qumirdb.oz is parsed only once. LoadAndCompose
        // still re-inlines and re-resolves it each call (the pending optimization), but
        // reparsing the source per call would cost seconds.
        auto composed = NQumir::NFrontend::LoadAndCompose(Loader_, module, Pragmas_);
        if (!composed) {
            ThrowTyping("compose", composed.error());
        }
        TExprPtr ast = std::move(composed->Ast);
        Resolver_.ApplyPragmas(composed->Pragmas);
        Resolver_.GetOrCreateRootScope()->RootLevel = false;

        if (auto err = Resolver_.Resolve(ast)) {
            ThrowTyping("resolve", *err);
        }
        if (auto transformed = NQumir::NTransform::Pipeline(ast, Resolver_, {}); !transformed) {
            ThrowTyping("annotate", transformed.error());
        }
        auto type = FindResultType(ast, resultName);
        if (!type) {
            throw NQumir::TError(
                "project type inference: annotator produced no type for '" +
                NQumir::NAst::NCore::PrintAst(expr) + "'");
        }
        return type;
    }

private:
    // Compose qumirdb.oz once and snapshot every function's (planner) return type by
    // name. Extern decls carry an empty `(block)` body (not nullptr), so we walk the
    // composed AST rather than TNameResolver::GetExternalFunctions (which filters on a
    // null body). We only ever look names up, so collecting non-extern fns too is benign.
    void EnsureExterns() {
        if (ExternsBuilt_ || LoadError_) {
            return;
        }
        ExternsBuilt_ = true;
        NQumir::TLocation loc{};
        TExprPtr module = std::make_shared<TBlockExpr>(loc,
            std::vector<TExprPtr>{std::make_shared<TUseExpr>(loc, "qumirdb")});
        auto composed = NQumir::NFrontend::LoadAndCompose(Loader_, module, Pragmas_);
        if (!composed) {
            return;
        }
        CollectReturns(composed->Ast);
    }

    void CollectReturns(const TExprPtr& node) {
        if (!node) {
            return;
        }
        if (auto fn = TMaybeNode<TFunDecl>(node);
            fn && fn.Cast()->RetType && !fn.Cast()->Name.empty())
        {
            ExternReturns_.emplace(fn.Cast()->Name, FromQumirType(fn.Cast()->RetType));
        }
        for (auto* child : node->MutableChildren()) {
            CollectReturns(*child);
        }
    }

    const std::vector<TPragma> Pragmas_{TPragma{"language", {"overloads"}, {}}};
    std::shared_ptr<NQumir::NRegistry::SystemModule> System_;
    NQumir::NFrontend::TSourceModuleLoader Loader_;
    NQumir::NSemantics::TNameResolver Resolver_;
    std::optional<NQumir::TError> LoadError_;
    uint64_t Counter_ = 0;
    bool ExternsBuilt_ = false;
    std::unordered_map<std::string, TTypePtr> ExternReturns_;
};

TTypingContext& Context() {
    static TTypingContext context;
    return context;
}

using NQumir::TLocation;

// --- small AST builders ---------------------------------------------------------
TExprPtr Ident(const std::string& n, TLocation loc) {
    return std::make_shared<TIdentExpr>(loc, n);
}
// Access `.Value`/`.Valid` of a (freshly cloned) nullable operand. Cloning keeps the
// AST a tree — the operand is referenced from both the guard and the result, so a shared
// node would be annotated/lowered twice.
TExprPtr FieldOf(const TExprPtr& obj, const char* field, TLocation loc) {
    return std::make_shared<TFieldAccessExpr>(loc, CloneExpr(obj), field);
}
TExprPtr BoolLit(bool v, TLocation loc) {
    return std::make_shared<TNumberExpr>(loc, v);
}
TExprPtr Bin(const char* op, TExprPtr a, TExprPtr b, TLocation loc) {
    return std::make_shared<TBinaryExpr>(loc, TOperator(std::string_view(op)),
        std::move(a), std::move(b));
}
TExprPtr Not(TExprPtr a, TLocation loc) {
    return std::make_shared<TUnaryExpr>(loc, TOperator(std::string_view("!")), std::move(a));
}
TExprPtr MakeNull(TLocation loc) {
    return std::make_shared<TCallExpr>(loc, Ident("qdb_sql_null", loc), std::vector<TExprPtr>{});
}

// A bare SQL NULL desugars to `qdb_sql_null()`; a missing CASE ELSE is a null child.
bool IsNullLiteral(const TExprPtr& e) {
    if (!e) {
        return true;
    }
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        if (auto id = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
            return id.Cast()->Name == "qdb_sql_null";
        }
    }
    return false;
}

// Calls whose result is NOT null-strict (never wrapped by the pass): they return a
// non-null bool or have bespoke NULL semantics handled in qumirdb.oz / elsewhere.
bool IsExcludedCall(const std::string& name) {
    return name == "qdb_is_null" || name == "qdb_is_true" ||
           name == "coalesce" || name == "nullif" || name == "nvl" || name == "ifnull";
}

bool IsComparison(TOperator op) {
    return op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=";
}
bool IsLogical(TOperator op) { return op == "&&" || op == "||"; }

TTypePtr ValueType(const TTypePtr& t) { return t ? UnwrapNullableType(t) : t; }
TTypePtr NullableQ(const TTypePtr& value) {
    return ToQumirType(std::make_shared<TNullable>(value));
}

// SQL null propagation: any nullable (or bare-NULL) operand makes the result Nullable.
TTypePtr Propagate(std::initializer_list<TTypePtr> operands, const TTypePtr& R) {
    if (!R) {
        return R;
    }
    bool nullable = IsNullableType(R);
    for (const auto& t : operands) {
        nullable = nullable || !t || IsNullableType(t);
    }
    TTypePtr base = UnwrapNullableType(R);
    return nullable ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(base)) : base;
}

bool IsInt(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TIntegerType>(t)); }
bool IsFloat(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TFloatType>(t)); }
bool IsIntLiteral(const TExprPtr& e) {
    auto n = TMaybeNode<TNumberExpr>(e);
    return n && !n.Cast()->IsFloat();
}

// Mirrors qumir NTypeAnnotation::CommonNumericType + WideningIntOK for the types qdb sees
// (all ints signed). Float dominates; two ints widen to the wider. TODO(reuse): call
// qumir's own BinaryNumericResultType once it is exposed (see NULL_PROPAGATION_PASS_PLAN.md).
TTypePtr CommonNumeric(const TTypePtr& a, const TTypePtr& b) {
    if (IsFloat(a) || IsFloat(b)) {
        return std::make_shared<TFloatType>();
    }
    auto ai = TMaybeType<TIntegerType>(a);
    auto bi = TMaybeType<TIntegerType>(b);
    if (ai && bi) {
        return bi.Cast()->BitWidth() >= ai.Cast()->BitWidth() ? b : a;
    }
    return a;
}

// Common value type of two CASE/`if` branches (SQL branch unification): numeric promotion
// (i32 then, i64 else -> i64), otherwise assume the branches already agree.
TTypePtr UnifyBranchTypes(const TTypePtr& a, const TTypePtr& b) {
    if (!a) return b ? UnwrapNullableType(b) : nullptr;
    if (!b) return UnwrapNullableType(a);
    TTypePtr va = UnwrapNullableType(a), vb = UnwrapNullableType(b);
    if ((IsInt(va) || IsFloat(va)) && (IsInt(vb) || IsFloat(vb))) {
        return CommonNumeric(va, vb);
    }
    return va;
}

// The plain (non-null-propagated) result value type of a binary op, matching qumir's
// AnnotateBinary: `/` -> float, `%` -> i64 (both int), `+ - *`/bitwise -> CommonNumeric,
// comparisons/logical -> bool. Integer-literal operands adopt the other operand's int
// type (qumir's RetypeIntegerLiteralOperands; the fit-check is approximated as "always").
TTypePtr BinaryValueType(TOperator op, const TExprPtr& le, const TExprPtr& re,
    const TTypePtr& lt, const TTypePtr& rt)
{
    if (IsComparison(op) || IsLogical(op)) {
        return std::make_shared<TBoolType>();
    }
    TTypePtr vl = UnwrapNamedType(ValueType(lt));
    TTypePtr vr = UnwrapNamedType(ValueType(rt));
    if (op == "+" && (TMaybeType<TStringType>(vl) || TMaybeType<TStringType>(vr))) {
        return std::make_shared<TStringType>(); // string concatenation
    }
    if (IsInt(vl) && IsInt(vr)) {
        if (IsIntLiteral(re)) vr = vl;
        else if (IsIntLiteral(le)) vl = vr;
    }
    if (op == "/") {
        return std::make_shared<TFloatType>();
    }
    if (op == "%") {
        return IsInt(vl) && IsInt(vr)
            ? std::static_pointer_cast<TType>(std::make_shared<TIntegerType>())
            : CommonNumeric(vl, vr);
    }
    return CommonNumeric(vl, vr);
}

// Lightweight, rule-based SQL type inference — no AST rewrite and no per-node qumirdb.oz
// compose. Types operators/calls "as if" ExpandNullable had run: a nullable operand makes
// the result Nullable[R]. Extern-call returns come from the resolver (ExternReturnType).
TTypePtr InferType(const TExprPtr& e, const TStructType& schema) {
    if (!e || IsNullLiteral(e)) {
        return nullptr; // bare/typed NULL — the enclosing node supplies the value type
    }
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return std::make_shared<TStringType>();
    }
    if (auto num = TMaybeNode<TNumberExpr>(e)) {
        // The ctor sets ->Type (bool / i64 / float); i32_literal overrides to i32.
        return num.Cast()->Type ? num.Cast()->Type
            : std::static_pointer_cast<TType>(std::make_shared<TIntegerType>());
    }
    if (auto id = TMaybeNode<TIdentExpr>(e)) {
        const std::string& n = id.Cast()->Name;
        for (const auto& [fn, ft] : schema.Fields) {
            if (fn == n) return ft;
        }
        return nullptr; // unknown here — tolerated when nested (null propagates)
    }
    if (auto bin = TMaybeNode<TBinaryExpr>(e)) {
        auto node =bin.Cast();
        TTypePtr lt = InferType(node->Left, schema);
        TTypePtr rt = InferType(node->Right, schema);
        return Propagate({lt, rt},
            BinaryValueType(node->Operator, node->Left, node->Right, lt, rt));
    }
    if (auto un = TMaybeNode<TUnaryExpr>(e)) {
        auto node =un.Cast();
        TTypePtr ot = InferType(node->Operand, schema);
        TTypePtr R = node->Operator == "!"
            ? std::static_pointer_cast<TType>(std::make_shared<TBoolType>())
            : ValueType(ot);
        return Propagate({ot}, R);
    }
    if (auto cst = TMaybeNode<TCastExpr>(e)) {
        auto node =cst.Cast();
        TTypePtr ot = InferType(node->Operand, schema);
        return Propagate({ot}, FromQumirType(node->Type));
    }
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        auto node =call.Cast();
        std::string name;
        if (auto cid = TMaybeNode<TIdentExpr>(node->Callee)) {
            name = cid.Cast()->Name;
        }
        if (name == "qdb_is_null" || name == "qdb_is_true") {
            return std::make_shared<TBoolType>(); // non-null bool regardless of args
        }
        std::vector<TTypePtr> at;
        for (const auto& a : node->Args) {
            at.push_back(InferType(a, schema));
        }
        TTypePtr ret = Context().ExternReturnType(name);
        bool nullable = false;
        for (const auto& t : at) nullable = nullable || !t || IsNullableType(t);
        if (!ret) {
            return nullptr;
        }
        TTypePtr base = UnwrapNullableType(ret);
        return (nullable || IsNullableType(ret))
            ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(base)) : base;
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node =ifp.Cast();
        TTypePtr tt = InferType(node->Then, schema);
        TTypePtr et = node->Else ? InferType(node->Else, schema) : nullptr;
        TTypePtr base = UnifyBranchTypes(tt, et);
        if (!base) {
            return nullptr;
        }
        bool nullable = !tt || !et || IsNullableType(tt) || IsNullableType(et);
        return nullable ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(base)) : base;
    }
    if (auto fa = TMaybeNode<TFieldAccessExpr>(e)) {
        auto node =fa.Cast();
        TTypePtr ot = InferType(node->Object, schema);
        if (ot && IsNullableType(ot)) {
            if (node->FieldName == "Value") return UnwrapNullableType(ot);
            if (node->FieldName == "Valid") return std::make_shared<TBoolType>();
        }
        return nullptr;
    }
    if (auto blk = TMaybeNode<TBlockExpr>(e)) {
        const auto& s = blk.Cast()->Stmts;
        return s.empty() ? nullptr : InferType(s.back(), schema);
    }
    return nullptr;
}

TTypePtr ResultTypeUnary(TOperator op, const TTypePtr& ot) {
    return op == "!" ? std::static_pointer_cast<TType>(std::make_shared<TBoolType>())
                     : ValueType(ot);
}

// Build `if(all-valid) cast(<apply unwrapped>, Nullable[R]) else null[R]`, guarding on
// every nullable operand's `.Valid` and applying the op to their `.Value`s. `R` is the
// plain result value type. Uses only `if`/`cast` (no block-local vars) — the shape the
// annotator and kernel lowering already handle; operands are cloned per reference.
// Bind `expr` to a fresh temp var (appended to `stmts`) and return an ident referencing
// it. `counter` is per-query (owned by the ExpandNullable call) — names must be unique
// within one expansion. Reading a temp's fields many times can't re-expand a compound
// operand, so this keeps the rewrite linear instead of 2^depth.
TExprPtr BindTemp(const TExprPtr& expr, std::vector<TExprPtr>& stmts, uint64_t& counter,
    TLocation loc)
{
    std::string name = "__nx" + std::to_string(counter++);
    auto var = std::make_shared<TVarStmt>(loc, name, nullptr);
    var->Init = expr;
    stmts.push_back(var);
    return Ident(name, loc);
}

TExprPtr BuildNullStrict(const std::vector<std::pair<TExprPtr, bool>>& operands,
    const std::function<TExprPtr(std::vector<TExprPtr>)>& apply,
    const TTypePtr& R, uint64_t& counter, TLocation loc)
{
    std::vector<TExprPtr> stmts;
    std::vector<TExprPtr> unwrapped;
    std::vector<TExprPtr> valids;
    for (const auto& [expr, nullable] : operands) {
        if (nullable) {
            auto ref = BindTemp(expr, stmts, counter, loc);
            unwrapped.push_back(FieldOf(ref, "Value", loc));
            valids.push_back(FieldOf(ref, "Valid", loc));
        } else {
            unwrapped.push_back(CloneExpr(expr));
        }
    }
    auto nq = NullableQ(R);
    TExprPtr guard = valids.front();
    for (size_t i = 1; i < valids.size(); ++i) {
        guard = Bin("&&", guard, valids[i], loc);
    }
    TExprPtr thenE = std::make_shared<TCastExpr>(loc, apply(unwrapped), nq);
    TExprPtr elseE = std::make_shared<TCastExpr>(loc, MakeNull(loc), nq);
    stmts.push_back(std::make_shared<TIfExpr>(loc, guard, thenE, elseE));
    return std::make_shared<TBlockExpr>(loc, std::move(stmts));
}

// SQL 3VL for AND/OR: evaluates both operands; a valid dominating value (FALSE for AND,
// TRUE for OR) wins over NULL. Each operand contributes (Valid, Value) plain bools;
// `ref == nullptr` is a bare NULL, non-nullable is always valid.
TExprPtr OpValid(const TExprPtr& ref, const TTypePtr& t, TLocation loc) {
    if (!t) return BoolLit(false, loc);
    if (IsNullableType(t)) return FieldOf(ref, "Valid", loc);
    return BoolLit(true, loc);
}
TExprPtr OpValue(const TExprPtr& ref, const TTypePtr& t, TLocation loc) {
    if (!t) return BoolLit(false, loc);
    if (IsNullableType(t)) return FieldOf(ref, "Value", loc);
    return CloneExpr(ref);
}
TExprPtr Build3VL(bool isAnd, const TExprPtr& a, const TTypePtr& at,
    const TExprPtr& b, const TTypePtr& bt, uint64_t& counter, TLocation loc)
{
    std::vector<TExprPtr> stmts;
    // Bind nullable operands to temps (read .Valid/.Value repeatedly below).
    TExprPtr ra = (at && IsNullableType(at)) ? BindTemp(a, stmts, counter, loc) : a;
    TExprPtr rb = (bt && IsNullableType(bt)) ? BindTemp(b, stmts, counter, loc) : b;
    auto boolN = NullableQ(std::make_shared<TBoolType>());
    auto nTrue = std::make_shared<TCastExpr>(loc, BoolLit(true, loc), boolN);
    auto nFalse = std::make_shared<TCastExpr>(loc, BoolLit(false, loc), boolN);
    auto nNull = std::make_shared<TCastExpr>(loc, MakeNull(loc), boolN);
    auto bothValid = Bin("&&", OpValid(ra, at, loc), OpValid(rb, bt, loc), loc);
    TExprPtr result;
    if (isAnd) {
        auto dominant = Bin("||",
            Bin("&&", OpValid(ra, at, loc), Not(OpValue(ra, at, loc), loc), loc),
            Bin("&&", OpValid(rb, bt, loc), Not(OpValue(rb, bt, loc), loc), loc), loc);
        result = std::make_shared<TIfExpr>(loc, dominant, nFalse,
            std::make_shared<TIfExpr>(loc, bothValid, nTrue, nNull));
    } else {
        auto dominant = Bin("||",
            Bin("&&", OpValid(ra, at, loc), OpValue(ra, at, loc), loc),
            Bin("&&", OpValid(rb, bt, loc), OpValue(rb, bt, loc), loc), loc);
        result = std::make_shared<TIfExpr>(loc, dominant, nTrue,
            std::make_shared<TIfExpr>(loc, bothValid, nFalse, nNull));
    }
    stmts.push_back(result);
    return std::make_shared<TBlockExpr>(loc, std::move(stmts));
}

// Coerce an `if`/CASE branch to Nullable[value] so all branches share one type: NULL ->
// null[value]; a plain branch is widened if needed, then wrapped via nullable_from_value;
// an already-nullable branch of a narrower type is widened through a null-strict cast.
TExprPtr CoerceBranch(const TExprPtr& branch, const TTypePtr& branchType, bool isNull,
    const TTypePtr& value, uint64_t& counter, TLocation loc)
{
    auto nq = NullableQ(value);
    if (isNull) {
        return std::make_shared<TCastExpr>(loc, branch ? branch : MakeNull(loc), nq);
    }
    auto tv = ToQumirType(value);
    if (!branchType || !IsNullableType(branchType)) {
        TExprPtr v = branch;
        if (branchType && TypeKey(branchType) != TypeKey(value)) {
            v = std::make_shared<TCastExpr>(loc, branch, tv); // widen plain value
        }
        return std::make_shared<TCastExpr>(v->Location, v, nq); // nullable_from_value
    }
    if (TypeKey(UnwrapNullableType(branchType)) == TypeKey(value)) {
        return branch; // already Nullable[value]
    }
    TExprPtr b = branch; // Nullable but narrower: widen the value under the validity guard
    return BuildNullStrict({{b, true}},
        [tv, loc](std::vector<TExprPtr> a) {
            return std::make_shared<TCastExpr>(loc, a[0], tv);
        }, value, counter, loc);
}

// Bottom-up rewrite. Mutates `e` in place, returns its planner type (`TNullable`-aware),
// or nullptr for a bare NULL leaf (the enclosing `if`/op supplies the type).
TTypePtr Expand(TExprPtr& e, const TStructType& inputType, uint64_t& counter) {
    if (IsNullLiteral(e)) {
        return nullptr;
    }
    // A string literal is a non-null `string` leaf. It is always the operand of a
    // parser-emitted `cast(lit, StringView)`; annotating the bare literal would wrap it in
    // a var (owned storage) and fail the lifetime validator, so type it directly.
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return std::make_shared<TStringType>();
    }

    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node = ifp.Cast();
        Expand(node->Cond, inputType, counter);
        if (!node->Then) node->Then = MakeNull(node->Location);
        if (!node->Else) node->Else = MakeNull(node->Location);
        TTypePtr thenT = Expand(node->Then, inputType, counter);
        TTypePtr elseT = Expand(node->Else, inputType, counter);
        const bool thenNull = !thenT, elseNull = !elseT;
        const bool thenNullable = thenNull || IsNullableType(thenT);
        const bool elseNullable = elseNull || IsNullableType(elseT);
        if (!thenNullable && !elseNullable) {
            return UnifyBranchTypes(thenT, elseT); // no null — unify (i32/i64 -> i64)
        }
        TTypePtr value = UnifyBranchTypes(thenT, elseT);
        if (!value) {
            return nullptr; // both branches NULL — untyped, leave for the parent
        }
        node->Then = CoerceBranch(node->Then, thenT, thenNull, value, counter, node->Location);
        node->Else = CoerceBranch(node->Else, elseT, elseNull, value, counter, node->Location);
        return std::make_shared<TNullable>(value);
    }

    if (auto bp = TMaybeNode<TBinaryExpr>(e)) {
        auto node = bp.Cast();
        TTypePtr lt = Expand(node->Left, inputType, counter);
        TTypePtr rt = Expand(node->Right, inputType, counter);
        TOperator op = node->Operator;
        const bool lNull = !lt || IsNullableType(lt);
        const bool rNull = !rt || IsNullableType(rt);
        if (IsLogical(op)) {
            if (!lNull && !rNull) {
                return std::make_shared<TBoolType>();
            }
            e = Build3VL(op == "&&", node->Left, lt, node->Right, rt, counter, node->Location);
            return std::make_shared<TNullable>(std::make_shared<TBoolType>());
        }
        if (!lNull && !rNull) {
            return BinaryValueType(op, node->Left, node->Right, lt, rt);
        }
        // Bare-NULL operand ⇒ the whole op is unconditionally NULL[R].
        if (!lt || !rt) {
            TTypePtr R = BinaryValueType(op, node->Left, node->Right,
                lt ? lt : rt, rt ? rt : lt);
            e = std::make_shared<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(R));
            return std::make_shared<TNullable>(R);
        }
        TTypePtr R = BinaryValueType(op, node->Left, node->Right, lt, rt);
        TExprPtr l = node->Left, r = node->Right;
        e = BuildNullStrict({{l, lNull}, {r, rNull}},
            [op, node](std::vector<TExprPtr> a) {
                return std::make_shared<TBinaryExpr>(node->Location, op, a[0], a[1]);
            }, R, counter, node->Location);
        return std::make_shared<TNullable>(R);
    }

    if (auto up = TMaybeNode<TUnaryExpr>(e)) {
        auto node = up.Cast();
        TTypePtr ot = Expand(node->Operand, inputType, counter);
        TOperator op = node->Operator;
        if (!ot || !IsNullableType(ot)) {
            return ot ? ResultTypeUnary(op, ot) : nullptr;
        }
        TTypePtr R = ResultTypeUnary(op, ot);
        TExprPtr operand = node->Operand;
        e = BuildNullStrict({{operand, true}},
            [op, node](std::vector<TExprPtr> a) {
                return std::make_shared<TUnaryExpr>(node->Location, op, a[0]);
            }, R, counter, node->Location);
        return std::make_shared<TNullable>(R);
    }

    if (auto cp = TMaybeNode<TCastExpr>(e)) {
        auto node = cp.Cast();
        TTypePtr ot = Expand(node->Operand, inputType, counter);
        TTypePtr target = FromQumirType(node->Type);
        if (!ot || !IsNullableType(ot)) {
            return ot ? target : nullptr;
        }
        TExprPtr operand = node->Operand;
        TTypePtr tt = node->Type;
        e = BuildNullStrict({{operand, true}},
            [tt, node](std::vector<TExprPtr> a) {
                return std::make_shared<TCastExpr>(node->Location, a[0], tt);
            }, target, counter, node->Location);
        return std::make_shared<TNullable>(target);
    }

    if (auto cl = TMaybeNode<TCallExpr>(e)) {
        auto node = cl.Cast();
        std::string name;
        if (auto id = TMaybeNode<TIdentExpr>(node->Callee)) {
            name = id.Cast()->Name;
        }
        std::vector<TTypePtr> argTypes;
        for (auto& arg : node->Args) {
            argTypes.push_back(Expand(arg, inputType, counter));
        }
        if (IsExcludedCall(name)) {
            return InferType(e, inputType); // e.g. is_null/is_true → non-null bool
        }
        bool anyNull = false;
        for (const auto& t : argTypes) {
            anyNull = anyNull || !t || IsNullableType(t);
        }
        TTypePtr ret = Context().ExternReturnType(name);
        if (!ret) {
            return InferType(e, inputType); // unknown extern: leave the call, best-effort type
        }
        TTypePtr R = UnwrapNullableType(ret);
        if (!anyNull) {
            return ret;
        }
        std::vector<std::pair<TExprPtr, bool>> operands;
        for (size_t i = 0; i < node->Args.size(); ++i) {
            operands.emplace_back(node->Args[i], !argTypes[i] || IsNullableType(argTypes[i]));
        }
        e = BuildNullStrict(operands,
            [node](std::vector<TExprPtr> a) {
                return std::make_shared<TCallExpr>(node->Location, node->Callee, std::move(a));
            }, R, counter, node->Location);
        return std::make_shared<TNullable>(R);
    }

    if (TMaybeNode<TIdentExpr>(e) || TMaybeNode<TNumberExpr>(e) ||
        TMaybeNode<TStringLiteralExpr>(e))
    {
        return InferType(e, inputType); // leaves — no rewrite
    }

    // Anything else (field access already produced by this pass, etc.): recurse into
    // children to rewrite nested nullables, then type by rule.
    for (auto* child : e->MutableChildren()) {
        Expand(*child, inputType, counter);
    }
    return InferType(e, inputType);
}

} // namespace

TTypePtr AnnotateExprType(const TExprPtr& expr, const TStructType& inputType) {
    if (!expr) {
        throw NQumir::TError("project type inference: null expression");
    }
    TTypePtr t = InferType(expr, inputType);
    if (!t) {
        throw NQumir::TError("project type inference: could not infer type of '" +
            NQumir::NAst::NCore::PrintAst(expr) + "'");
    }
    return t;
}

namespace {
// Structural check for a bare NULL (qdb_sql_null) anywhere in the tree.
bool ContainsNull(const TExprPtr& e) {
    if (IsNullLiteral(e)) {
        return true;
    }
    for (const auto& child : e->Children()) {
        if (ContainsNull(child)) return true;
    }
    return false;
}
} // namespace

std::pair<TExprPtr, TTypePtr> ExpandNullable(const TExprPtr& expr, const TStructType& inputType) {
    // Fast path: with no nullable column and no bare NULL there is nothing to propagate,
    // so leave the expression untouched (non-nullable path stays ordinary).
    bool anyNullable = false;
    for (const auto& [name, type] : inputType.Fields) {
        if (IsNullableType(type)) { anyNullable = true; break; }
    }
    if (!anyNullable && !ContainsNull(expr)) {
        return {expr, InferType(expr, inputType)};
    }
    // Rewrite a clone — this runs at kernel build on the plan's shared expression, which
    // must stay intact (a node may be lowered into more than one kernel).
    TExprPtr e = CloneExpr(expr);
    uint64_t counter = 0; // per-query temp-name counter (unique within this expansion)
    TTypePtr t = Expand(e, inputType, counter);
    return {e, t};
}

} // namespace NQdb::NKernel
