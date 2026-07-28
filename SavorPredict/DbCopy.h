#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace savor::predict {

struct PrepareDbOptions {
    std::filesystem::path source = "D:/SoaSimDBDebug";
    std::filesystem::path dest = "D:/SavorPredictDB";
    bool overwrite = false;
};

int run_prepare_db(const PrepareDbOptions& options, std::ostream& out, std::ostream& err);

bool compute_file_sha256_streaming(
    const std::filesystem::path& path,
    std::string* sha256,
    std::ostream& err);

} // namespace savor::predict
