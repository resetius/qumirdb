#include "const_fold.h"

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/modules/system/system.h>

#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/eval.h>

#include <qumir/parser/ast.h>

#include <charconv>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

constexpr const char* EvalFn = "__const_fold_eval__";

// TODO(perf): reparses qumirdb.oz per call; hoist the loader into a context.
std::optional<std::string> EvalClosed(const TExprPtr& expr, const TTypePtr& retType) {
    NQumir::TLocation loc{};
    const std::vector<TPragma> pragmas{TPragma{"language", {"overloads"}, {}}};

    NQumir::NFrontend::TSourceModuleLoader loader;
    if (!loader.RegisterSourceModule(
            std::string(QDB_SOURCE_DIR) + "/modules/qumirdb.oz")) {
        return std::nullopt;
    }

    auto body = std::make_shared<TBlockExpr>(
        loc, std::vector<TExprPtr>{expr});
    auto fn = std::make_shared<TFunDecl>(
        loc, EvalFn, std::vector<TGenericParam>{}, std::vector<TParam>{},
        std::move(body), retType);
    TExprPtr chunk = std::make_shared<TBlockExpr>(
        loc, std::vector<TExprPtr>{
            std::make_shared<TUseExpr>(loc, "qumirdb"),
            std::move(fn),
        });

    auto composed = NQumir::NFrontend::LoadAndCompose(loader, chunk, pragmas);
    if (!composed) {
        return std::nullopt;
    }
    TExprPtr ast = std::move(composed->Ast);

    NQumir::NIR::TModule module;
    NQumir::NIR::TBuilder builder(module);
    NQumir::NSemantics::TNameResolver resolver;
    auto system = std::make_shared<NQumir::NRegistry::SystemModule>();
    resolver.RegisterModule(system.get());
    resolver.ApplyPragmas(composed->Pragmas);
    resolver.GetOrCreateRootScope()->RootLevel = false;

    if (resolver.Resolve(ast)) {
        return std::nullopt;
    }
    if (!NQumir::NTransform::Pipeline(ast, resolver, {})) {
        return std::nullopt;
    }

    NQumir::NIR::TAstLowerer lowerer(module, builder, resolver);
    if (!lowerer.LowerTop(ast)) {
        return std::nullopt;
    }

    NQumir::NIR::TFunction* entry = nullptr;
    for (auto& function : module.Functions) {
        if (function.Name == EvalFn) {
            entry = &function;
            break;
        }
    }
    if (!entry) {
        return std::nullopt;
    }

    std::ostringstream out;
    std::istringstream in;
    NQumir::NIR::TInterpreter interpreter(module, out, in);
    try {
        return interpreter.Eval(*entry, {}, {});
    } catch (...) {
        return std::nullopt;
    }
}

TExprPtr LiteralFromResult(const std::string& s) {
    NQumir::TLocation loc{};
    if (s == "true") {
        return std::make_shared<TNumberExpr>(loc, true);
    }
    if (s == "false") {
        return std::make_shared<TNumberExpr>(loc, false);
    }
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    int64_t intValue = 0;
    if (auto [p, ec] = std::from_chars(begin, end, intValue); ec == std::errc{} && p == end) {
        return std::make_shared<TNumberExpr>(loc, intValue);
    }
    double floatValue = 0.0;
    if (auto [p, ec] = std::from_chars(begin, end, floatValue); ec == std::errc{} && p == end) {
        return std::make_shared<TNumberExpr>(loc, floatValue);
    }
    return nullptr;
}

} // namespace

TExprPtr ConstFold(TExprPtr expr) {
    if (!expr || !FindUnboundVars(expr).empty()) {
        return expr;
    }
    // A closed expression can be typed against an empty input schema; expansion
    // rewrites SQL nullability/Decimal and yields the result type used to wrap it.
    auto [rewritten, rewrittenType] =
        NKernel::ExpandKernelExpr(CloneExpr(expr), TStructType({}));
    if (!rewrittenType) {
        return expr;
    }
    auto result = EvalClosed(rewritten, rewrittenType);
    if (!result) {
        return expr;
    }
    auto literal = LiteralFromResult(*result);
    return literal ? literal : expr;
}

} // namespace NQdb
