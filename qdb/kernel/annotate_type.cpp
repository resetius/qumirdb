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

// Planner column type -> the qumir type the annotator should see. De-risk boundary:
// nullability is dropped (kernels still materialize scalar values), so a NQdb::TNullable
// column is passed as its plain underlying type — the annotator resolves ordinary
// operators, not the generic Nullable[T] overloads. String columns are StringView in
// kernels. (Increment B will pass Nullable[T] and exercise those overloads.)
TTypePtr ToQumirType(const TTypePtr& type) {
    if (!type) {
        return type;
    }
    if (IsNullableType(type)) {
        return ToQumirType(UnwrapNullableType(type));
    }
    if (TMaybeType<TStringType>(type)) {
        return std::make_shared<TNamedType>("StringView", nullptr);
    }
    return type;
}

// Annotator result -> planner type. De-risk boundary: unwrap Nullable, and map the
// StringView named type back to a plain string so downstream treats it as a string
// column.
TTypePtr FromQumirType(const TTypePtr& type) {
    TTypePtr t = type;
    if (auto named = TMaybeType<TNamedType>(t);
        named && named.Cast()->Name == "Nullable" && !named.Cast()->TypeArgs.empty())
    {
        t = named.Cast()->TypeArgs.front().Type;
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

} // namespace NQdb::NKernel
