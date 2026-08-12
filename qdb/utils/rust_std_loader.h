#pragma once

namespace NQdb::NUtils {

// Makes the dynamic Rust standard library visible through the process symbol
// table. The loader must outlive every JIT module that can call Rust code.
class TRustStdLoader {
public:
    TRustStdLoader();
    ~TRustStdLoader();

    TRustStdLoader(const TRustStdLoader&) = delete;
    TRustStdLoader& operator=(const TRustStdLoader&) = delete;
    TRustStdLoader(TRustStdLoader&&) = delete;
    TRustStdLoader& operator=(TRustStdLoader&&) = delete;

private:
    void* Handle_ = nullptr;
};

} // namespace NQdb::NUtils
