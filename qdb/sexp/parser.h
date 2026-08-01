#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include <qumir/parser/core/parser.h>

#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/operator.h>

namespace NQdb {
namespace NSexp {

struct TRelParserOptions {
    // Called when (rel source "path") is encountered. If null, source parsing fails.
    std::function<TOperatorPtr(std::string_view path, NQumir::TLocation)> SourceFactory;
    // Shared by the (query (cte <id> ...) ...) envelope parser (which fills it) and
    // the (rel cte-ref <id>) parser (which resolves against it). Null disables CTEs.
    std::shared_ptr<std::unordered_map<uint32_t, TCteDefinitionPtr>> CteRegistry;
};

NQumir::NAst::NCore::TNodeParserMap MakeRelParsers(TRelParserOptions options = {});

} // namespace NSexp
} // namespace NQdb
