#include <qdb/utils/rust_std_loader.h>

#include <dlfcn.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace NQdb::NUtils {

namespace {

std::string RustTargetLibDir() {
    using TPipe = std::unique_ptr<FILE, decltype(&::pclose)>;
    TPipe pipe(::popen("rustc --print target-libdir", "r"), &::pclose);
    if (!pipe) {
        throw std::runtime_error("cannot execute rustc");
    }

    std::string result;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe.get())) {
        result += buffer;
    }
    const int status = ::pclose(pipe.release());
    if (status != 0) {
        throw std::runtime_error("rustc --print target-libdir failed");
    }

    while (!result.empty() &&
        (result.back() == '\n' || result.back() == '\r'))
    {
        result.pop_back();
    }
    if (result.empty()) {
        throw std::runtime_error("rustc returned an empty target library directory");
    }
    return result;
}

std::filesystem::path FindRustStd() {
#if defined(__APPLE__)
    constexpr std::string_view suffix = ".dylib";
#else
    constexpr std::string_view suffix = ".so";
#endif

    const std::string targetLibDir = RustTargetLibDir();
    for (const auto& entry : std::filesystem::directory_iterator(targetLibDir)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("libstd-") && name.ends_with(suffix)) {
            return entry.path();
        }
    }
    throw std::runtime_error(
        "cannot find the Rust std dynamic library in " + targetLibDir);
}

} // namespace

TRustStdLoader::TRustStdLoader() {
    const std::filesystem::path rustStd = FindRustStd();
    ::dlerror();
    Handle_ = ::dlopen(rustStd.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!Handle_) {
        const char* error = ::dlerror();
        throw std::runtime_error(
            "cannot load " + rustStd.string() + ": " +
            (error ? std::string(error) : "unknown dlopen error"));
    }
}

TRustStdLoader::~TRustStdLoader() {
    if (Handle_) {
        ::dlclose(Handle_);
    }
}

} // namespace NQdb::NUtils
