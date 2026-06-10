#pragma once
#include <vector>
#include <string>
#include <sstream>

namespace string {
    static inline std::vector<std::string> splitStringByNewline(const std::string& s) {
        std::vector<std::string> lines;
        std::stringstream ss(s); // Create a stringstream from the input string
        std::string line;

        // Read lines from the stringstream using '\n' as the delimiter
        while (std::getline(ss, line, '\n')) {
            lines.push_back(line);
        }
        return lines;
    }
}