#include <qdb/utils/regex.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <array>
#include <limits>
#include <new>
#include <string>

namespace NQdb::NUtils {

namespace {

std::string ErrorMessage(int code) {
    std::array<PCRE2_UCHAR, 256> buffer{};
    const int size = pcre2_get_error_message(code, buffer.data(), buffer.size());
    if (size < 0) {
        return "PCRE2 error " + std::to_string(code);
    }
    return std::string(reinterpret_cast<const char*>(buffer.data()), size);
}

} // namespace

struct TRegexContext::TImpl {
    ~TImpl() {
        pcre2_match_data_free(Data);
        pcre2_match_context_free(MatchContext);
        pcre2_jit_stack_free(JitStack);
    }

    pcre2_match_data* Ensure(uint32_t count) {
        if (!MatchContext) {
            MatchContext = pcre2_match_context_create(nullptr);
            if (!MatchContext) {
                throw std::bad_alloc();
            }
            JitStack = pcre2_jit_stack_create(32 * 1024, 512 * 1024, nullptr);
            if (JitStack) {
                pcre2_jit_stack_assign(MatchContext, nullptr, JitStack);
            }
        }
        if (count <= Capacity) {
            return Data;
        }
        pcre2_match_data_free(Data);
        Data = pcre2_match_data_create(count, nullptr);
        if (!Data) {
            Capacity = 0;
            throw std::bad_alloc();
        }
        Capacity = count;
        return Data;
    }

    pcre2_match_data* Data = nullptr;
    pcre2_match_context* MatchContext = nullptr;
    pcre2_jit_stack* JitStack = nullptr;
    uint32_t Capacity = 0;
};

struct TRegex::TImpl {
    explicit TImpl(std::string_view pattern) {
        int error = 0;
        PCRE2_SIZE offset = 0;
        Code = pcre2_compile(
            reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(),
            0, &error, &offset, nullptr);
        if (!Code) {
            throw TRegexError(
                ErrorMessage(error) + " at offset " + std::to_string(offset));
        }
        if (pcre2_pattern_info(Code, PCRE2_INFO_CAPTURECOUNT, &CaptureCount) != 0) {
            pcre2_code_free(Code);
            Code = nullptr;
            throw TRegexError("cannot read PCRE2 capture count");
        }
        // Best effort: pcre2_match falls back to the interpreter when JIT is
        // unavailable for a valid pattern or in the linked PCRE2 build.
        pcre2_jit_compile(Code, PCRE2_JIT_COMPLETE);
    }

    ~TImpl() {
        pcre2_code_free(Code);
    }

    pcre2_code* Code = nullptr;
    uint32_t CaptureCount = 0;
};

std::optional<TRegexCapture> TRegexMatch::Capture(size_t index) const {
    if (index >= Count_) {
        return std::nullopt;
    }
    const size_t begin = Offsets_[index * 2];
    const size_t end = Offsets_[index * 2 + 1];
    if (begin == std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return TRegexCapture{.Begin = begin, .End = end};
}

TRegex::TRegex(std::string_view pattern)
    : Impl_(std::make_unique<TImpl>(pattern))
{}

TRegex::~TRegex() = default;

TRegexContext::TRegexContext() = default;

TRegexContext::~TRegexContext() = default;

TRegexContext::TImpl& TRegexContext::GetImpl() {
    if (!Impl_) {
        Impl_ = std::make_unique<TImpl>();
    }
    return *Impl_;
}

std::optional<TRegexMatch> TRegex::Search(
    TRegexContext& context, std::string_view subject) const
{
    const char empty = '\0';
    if (!subject.data()) {
        subject = std::string_view(&empty, 0);
    }
    const uint32_t count = Impl_->CaptureCount + 1;
    auto& impl = context.GetImpl();
    auto* matchData = impl.Ensure(count);
    const int result = pcre2_match(
        Impl_->Code,
        reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(),
        0, 0, matchData, impl.MatchContext);
    if (result == PCRE2_ERROR_NOMATCH) {
        return std::nullopt;
    }
    if (result < 0) {
        throw TRegexError(ErrorMessage(result));
    }
    static_assert(sizeof(PCRE2_SIZE) == sizeof(size_t));
    return TRegexMatch(
        reinterpret_cast<const size_t*>(pcre2_get_ovector_pointer(matchData)),
        count);
}

} // namespace NQdb::NUtils
