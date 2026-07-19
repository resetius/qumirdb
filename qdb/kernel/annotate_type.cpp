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

#include <memory>
#include <optional>
#include <string>
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
    const std::vector<TPragma> Pragmas_{TPragma{"language", {"overloads"}, {}}};
    std::shared_ptr<NQumir::NRegistry::SystemModule> System_;
    NQumir::NFrontend::TSourceModuleLoader Loader_;
    NQumir::NSemantics::TNameResolver Resolver_;
    std::optional<NQumir::TError> LoadError_;
    uint64_t Counter_ = 0;
};

TTypingContext& Context() {
    static TTypingContext context;
    return context;
}

} // namespace

TTypePtr AnnotateExprType(const TExprPtr& expr, const TStructType& inputType) {
    if (!expr) {
        throw NQumir::TError("project type inference: null expression");
    }
    return FromQumirType(Context().Annotate(expr, inputType));
}

namespace {

// A bare SQL NULL desugars to `qdb_sql_null()` (see the SQL parser); a missing CASE
// ELSE arrives as a null child pointer. Both are "untyped null" branches.
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

// Cheap structural check: does the subtree contain anything this pass must touch
// (an `if`/CASE or a bare NULL)? Lets us skip annotating branches that are plain
// value expressions with no null semantics.
bool NeedsRewrite(const TExprPtr& e) {
    if (!e) {
        return true; // a null child == implicit ELSE NULL
    }
    if (TMaybeNode<TIfExpr>(e) || IsNullLiteral(e)) {
        return true;
    }
    for (const auto& child : e->Children()) {
        if (NeedsRewrite(child)) {
            return true;
        }
    }
    return false;
}

TExprPtr MakeNull(const NQumir::TLocation& loc) {
    return std::make_shared<TCallExpr>(
        loc, std::make_shared<TIdentExpr>(loc, "qdb_sql_null"),
        std::vector<TExprPtr>{});
}

// Turn a branch into Nullable[T]. `nullableQumir` is the concrete `<named Nullable
// [T]>` type node; the cast resolves to nullable_from_null (null branch) or
// nullable_from_value (plain T) in qumirdb.oz. Already-nullable branches pass through.
TExprPtr CoerceBranch(
    const TExprPtr& branch, bool isNull, bool isNullable,
    const TTypePtr& nullableQumir, const NQumir::TLocation& loc)
{
    if (isNull) {
        return std::make_shared<TCastExpr>(loc, branch ? branch : MakeNull(loc), nullableQumir);
    }
    if (isNullable) {
        return branch; // already Nullable[T]
    }
    return std::make_shared<TCastExpr>(branch->Location, branch, nullableQumir);
}

// Post-order rewrite. Returns the (planner) type of the possibly-rewritten `e`, or
// nullptr for an untyped NULL branch — the enclosing `if` supplies T.
TTypePtr RewriteIfs(TExprPtr& e, const TStructType& inputType) {
    if (IsNullLiteral(e)) {
        return nullptr; // defer typing to the parent `if`
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node = ifp.Cast();
        RewriteIfs(node->Cond, inputType); // rewrite nested ifs in the condition
        if (!node->Then) {
            node->Then = MakeNull(node->Location);
        }
        if (!node->Else) {
            node->Else = MakeNull(node->Location);
        }
        TTypePtr thenT = RewriteIfs(node->Then, inputType);
        TTypePtr elseT = RewriteIfs(node->Else, inputType);
        const bool thenNull = !thenT;
        const bool elseNull = !elseT;
        const bool thenNullable = thenNull || IsNullableType(thenT);
        const bool elseNullable = elseNull || IsNullableType(elseT);
        if (!thenNullable && !elseNullable) {
            return thenT; // no null anywhere — let the annotator unify the branches
        }
        // T from whichever branch carries a value type (unwrap if already nullable).
        TTypePtr value = thenT ? (IsNullableType(thenT) ? UnwrapNullableType(thenT) : thenT)
                       : elseT ? (IsNullableType(elseT) ? UnwrapNullableType(elseT) : elseT)
                       : nullptr;
        if (!value) {
            return nullptr; // both branches NULL: genuinely untyped, leave for the parent
        }
        auto nullableQumir = ToQumirType(std::make_shared<TNullable>(value));
        node->Then = CoerceBranch(node->Then, thenNull, thenNullable, nullableQumir, node->Location);
        node->Else = CoerceBranch(node->Else, elseNull, elseNullable, nullableQumir, node->Location);
        return std::make_shared<TNullable>(value);
    }
    // Generic node: descend only into children that carry null/`if` semantics, then
    // type the whole node once (avoids annotating every leaf of a plain expression).
    for (auto* child : e->MutableChildren()) {
        if (NeedsRewrite(*child)) {
            RewriteIfs(*child, inputType);
        }
    }
    return AnnotateExprType(e, inputType);
}

} // namespace

TExprPtr NormalizeNullBranches(const TExprPtr& expr, const TStructType& inputType) {
    if (!NeedsRewrite(expr)) {
        return expr; // fast path: no NULL/`if`, non-nullable expression untouched
    }
    TExprPtr e = expr;
    RewriteIfs(e, inputType);
    return e;
}

} // namespace NQdb::NKernel
