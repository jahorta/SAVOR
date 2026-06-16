#pragma once

#include <string>
#include <utility>

namespace savorqt::db {

enum class ServiceErrorKind {
    Ok,
    NotFound,
    Unavailable,
    InvalidInput,
    Failed,
};

struct ServiceError {
    ServiceErrorKind kind = ServiceErrorKind::Ok;
    std::string message;
};

inline constexpr const char* kSavorDbRuntimeUnavailableMessage = "SavorDb runtime is unavailable";

template <typename T>
struct ServiceResult {
    bool ok = false;
    T value{};
    ServiceError error{};

    static ServiceResult Ok(T value) {
        ServiceResult result{};
        result.ok = true;
        result.value = std::move(value);
        return result;
    }

    static ServiceResult Err(ServiceError error) {
        ServiceResult result{};
        result.ok = false;
        result.error = std::move(error);
        return result;
    }
};

template <>
struct ServiceResult<void> {
    bool ok = false;
    ServiceError error{};

    static ServiceResult Ok() {
        ServiceResult result{};
        result.ok = true;
        return result;
    }

    static ServiceResult Err(ServiceError error) {
        ServiceResult result{};
        result.ok = false;
        result.error = std::move(error);
        return result;
    }
};

} // namespace savorqt::db
