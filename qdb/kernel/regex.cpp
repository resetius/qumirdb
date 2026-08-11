#include <qdb/kernel/regex.h>

#include <qdb/modules/qumirdb_runtime.h>

#include <regex>
#include <stdexcept>
#include <utility>

namespace NQdb::NKernel {

size_t TRegexRegistry::Register(
    std::string pattern, std::string replacement)
{
    for (size_t i = 0; i < Specs_.size(); ++i) {
        if (Specs_[i].Pattern == pattern &&
            Specs_[i].Replacement == replacement)
        {
            return i;
        }
    }

    std::shared_ptr<TCompiledRegex> compiled;
    try {
        compiled = std::make_shared<TCompiledRegex>(pattern, replacement);
    } catch (const std::regex_error& e) {
        throw std::runtime_error(
            "REGEXP_REPLACE invalid pattern: " + std::string(e.what()));
    }
    const size_t id = Specs_.size();
    Specs_.push_back({std::move(pattern), std::move(replacement)});
    Handles_.push_back(compiled.get());
    Compiled_.push_back(std::move(compiled));
    return id;
}

const std::vector<TRegexSpec>& TRegexRegistry::Specs() const {
    return Specs_;
}

void** TRegexRegistry::Handles() {
    return Handles_.empty() ? nullptr : Handles_.data();
}

} // namespace NQdb::NKernel
