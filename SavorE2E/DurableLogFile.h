#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "Cli.h"

namespace savor::e2e {

class DurableLogFile {
public:
    bool Open(const CliOptions& options, const std::string& scenario_name, std::string* error_out);
    void AppendLine(const std::string& line);
    const std::filesystem::path& path() const;
    bool is_open() const;

private:
    std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream stream_;
};

} // namespace savor::e2e
