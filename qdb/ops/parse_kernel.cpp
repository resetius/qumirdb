#include <qdb/ops/parse_kernel.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/type_annotation/type_annotation.h>

#include <sstream>

namespace NQqb {
namespace NInternal {

std::string TypeToCoreStr(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    if (!type) {
        return "i64";
    }
    if (auto t = TMaybeType<TIntegerType>(type)) {
        return t.Cast()->ToString(); // "i8", "i32", "i64", "u8", "u64", etc.
    }
    if (TMaybeType<TFloatType>(type)) {
        return "f64";
    }
    if (TMaybeType<TBoolType>(type)) {
        return "bool";
    }
    if (TMaybeType<TStringType>(type)) {
        return "string";
    }
    if (TMaybeType<TSymbolType>(type)) {
        return "char";
    }
    return "i64";
}

std::string WrapInFunction(
    const NQumir::NAst::TStructType& inputType,
    const std::string& expr)
{
    std::string src = "(block (fun <kernel> void (";
    for (const auto& [name, type] : inputType.Fields) {
        src += "(var " + name + " " + TypeToCoreStr(type) + ") ";
    }
    src += ") () (block ";
    src += expr;
    src += ")))";
    return src;
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
ParseAndAnnotate(
    const NQumir::NAst::TStructType& inputType,
    const std::string& expr)
{
    using namespace NQumir;

    std::string src = WrapInFunction(inputType, expr);
    std::istringstream ss(src);
    NAst::NCore::TTokenStream tokens(ss);
    NAst::NCore::TParser parser;

    auto parseResult = parser.Parse(tokens);
    if (!parseResult) {
        return std::unexpected(parseResult.error());
    }

    NSemantics::TNameResolver resolver;
    if (auto err = resolver.Resolve(*parseResult)) {
        return std::unexpected(*err);
    }

    NTypeAnnotation::TTypeAnnotator annotator(resolver);
    auto annotated = annotator.Annotate(*parseResult);
    if (!annotated) {
        return std::unexpected(annotated.error());
    }

    // Extract the TFunDecl from (block (fun <kernel> ...))
    auto* block = static_cast<NAst::TBlockExpr*>(annotated->get());
    if (block->Stmts.empty()) {
        return std::unexpected(TError("kernel wrapper: empty block"));
    }
    return block->Stmts[0]; // the TFunDecl
}

} // namespace NInternal
} // namespace NQqb
