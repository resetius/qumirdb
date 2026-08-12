#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace NQdb::NUtils {

class TRegexError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct TRegexCapture {
    size_t Begin = 0;
    size_t End = 0;
};

// Match offsets remain valid until the next Search() with the same context.
class TRegexMatch {
public:
    std::optional<TRegexCapture> Capture(size_t index) const;

private:
    friend class TRegex;

    TRegexMatch(const size_t* offsets, size_t count)
        : Offsets_(offsets)
        , Count_(count)
    {}

    const size_t* Offsets_ = nullptr;
    size_t Count_ = 0;
};

class TRegexContext {
public:
    TRegexContext();
    ~TRegexContext();

    TRegexContext(const TRegexContext&) = delete;
    TRegexContext& operator=(const TRegexContext&) = delete;

private:
    friend class TRegex;

    struct TImpl;
    TImpl& GetImpl();
    std::unique_ptr<TImpl> Impl_;
};

// Immutable compiled PCRE2 pattern. Search() is safe to call concurrently.
class TRegex {
public:
    explicit TRegex(std::string_view pattern);
    ~TRegex();

    TRegex(const TRegex&) = delete;
    TRegex& operator=(const TRegex&) = delete;

    std::optional<TRegexMatch> Search(
        TRegexContext& context, std::string_view subject) const;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

} // namespace NQdb::NUtils
