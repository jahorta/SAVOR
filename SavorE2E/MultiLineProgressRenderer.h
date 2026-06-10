#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <iosfwd>
#include <string>
#include <vector>

namespace savor::e2e {

class MultiLineProgressRenderer {
public:
    explicit MultiLineProgressRenderer(std::size_t line_count = 0);

    void SetLine(std::size_t index, std::string value);
    void SetLines(std::vector<std::string> lines);
    const std::vector<std::string>& Lines() const;
    void Render(std::ostream& out);
    bool RenderIfDue(
        std::ostream& out,
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds cadence);
    void WriteEventLine(std::ostream& out, const std::string& line);
    void WriteEventLines(std::ostream& out, const std::vector<std::string>& lines);
    void WriteEventLines(std::ostream& out, const std::deque<std::string>& lines);

private:
    template <typename TLineContainer>
    void WriteEventLinesImpl(std::ostream& out, const TLineContainer& event_lines);
    void RenderLinesAtCursor(std::ostream& out);

    std::vector<std::string> lines_;
    std::size_t rendered_line_count_ = 0;
    std::chrono::steady_clock::time_point last_render_at_{};
    bool has_rendered_ = false;
};

} // namespace savor::e2e
