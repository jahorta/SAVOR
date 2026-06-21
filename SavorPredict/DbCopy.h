#pragma once

#include <filesystem>
#include <iosfwd>

namespace savor::predict {

struct PrepareDbOptions {
    std::filesystem::path source = "D:/SoaSimDBDebug";
    std::filesystem::path dest = "D:/SavorPredictDB";
    bool overwrite = false;
};

int run_prepare_db(const PrepareDbOptions& options, std::ostream& out, std::ostream& err);

} // namespace savor::predict
