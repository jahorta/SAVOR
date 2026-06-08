#include "DurableLogFile.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace simcore::e2e {
namespace {

std::string SanitizeFileStem(std::string value) {
    for (auto& ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '-'
            || ch == '_';
        if (!ok) {
            ch = '_';
        }
    }
    return value.empty() ? "scenario" : value;
}

std::string TimestampForFileName() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%dT%H%M%S")
        << std::setw(3) << std::setfill('0') << millis
        << "Z";
    return oss.str();
}

std::string TimestampForLine() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << millis
        << "Z";
    return oss.str();
}

} // namespace

bool DurableLogFile::Open(const CliOptions& options, const std::string& scenario_name, std::string* error_out) {
    const auto root = options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default");
    const auto log_dir = root / "log";
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating durable log directory: " + log_dir.string() + " error=" + ec.message();
        }
        return false;
    }

    path_ = log_dir / (TimestampForFileName() + "-" + SanitizeFileStem(scenario_name) + "-durable.log");
    stream_.open(path_, std::ios::out | std::ios::app);
    if (!stream_.is_open()) {
        if (error_out != nullptr) {
            *error_out = "failed opening durable log file: " + path_.string();
        }
        return false;
    }
    return true;
}

void DurableLogFile::AppendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) {
        return;
    }
    stream_ << '[' << TimestampForLine() << "] " << line << '\n';
    stream_.flush();
}

const std::filesystem::path& DurableLogFile::path() const {
    return path_;
}

bool DurableLogFile::is_open() const {
    return stream_.is_open();
}

} // namespace simcore::e2e
