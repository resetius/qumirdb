#pragma once

// Internal helper: wraps a core-lang expression in a function, parses and annotates it.
// Used by MakeFilter and MakeProject.

#include <qumir/error.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <expected>
#include <string>

namespace NQdb {
namespace NInternal {

// Converts a qumir TTypePtr to its core-lang type string.
// e.g. TIntegerType{I64} → "i64", TFloatType → "f64"
std::string TypeToCoreStr(const NQumir::NAst::TTypePtr& type);

// Generates source for:
//   (block (fun <kernel> void ((field1 T1) ...) () (block EXPR)))
// where fields come from a TStructType.
// Returns the generated source string.
std::string WrapInFunction(
    const NQumir::NAst::TStructType& inputType,
    const std::string& expr);

// Parses WrapInFunction output, resolves names, annotates types.
// Returns the annotated TFunDecl node.
std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
ParseAndAnnotate(
    const NQumir::NAst::TStructType& inputType,
    const std::string& expr);

} // namespace NInternal
} // namespace NQdb
