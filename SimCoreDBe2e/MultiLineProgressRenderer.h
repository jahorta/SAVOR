#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace simcore::e2e {

class MultiLineProgressRenderer {
public:
    explicit MultiLineProgressRenderer(std::size_t line_count = 0);

    void SetLine(std::size_t index, std::string value);
    void SetLines(std::vector<std::string> lines);
    const std::vector<std::string>& Lines() const;
    void Render(std::ostream& out);
    void WriteEventLine(std::ostream& out, const std::string& line);

private:
    void RenderLinesAtCursor(std::ostream& out);

    std::vector<std::string> lines_;
    std::size_t rendered_line_count_ = 0;
    bool has_rendered_ = false;
};

} // namespace simcore::e2e
