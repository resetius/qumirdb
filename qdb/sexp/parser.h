#pragma once

#include <functional>

#include <qumir/parser/core/parser.h>

#include <qdb/plan/ops/operator.h>

namespace NQqb {
namespace NSexp {

struct TRelParserOptions {
    // Called when (rel source "path") is encountered. If null, source parsing fails.
    std::function<TOperatorPtr(std::string_view path, NQumir::TLocation)> SourceFactory;
};

NQumir::NAst::NCore::TNodeParserMap MakeRelParsers(TRelParserOptions options = {});

} // namespace NSexp
} // namespace NQqb
