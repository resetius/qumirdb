#pragma once

#include <functional>

#include <qumir/parser/core/parser.h>

#include <qdb/ops/operator.h>

namespace NQqb {
namespace NSexp {

struct TRelParserOptions {
    // Called when (rel source) is encountered. If null, source parsing fails.
    std::function<TOperatorPtr(NQumir::TLocation)> SourceFactory;
};

NQumir::NAst::NCore::TNodeParserMap MakeRelParsers(TRelParserOptions options = {});

} // namespace NSexp
} // namespace NQqb
