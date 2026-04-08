#pragma once

#include <string>

struct ValidationResult {
    std::string name;
    bool passed = false;
    std::string message;
};
