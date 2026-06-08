#include "MultiLineProgressRenderer.h"

#include <ostream>
#include <utility>

namespace simcore::e2e {

MultiLineProgressRenderer::MultiLineProgressRenderer(std::size_t line_count)
    : lines_(line_count) {
}

void MultiLineProgressRenderer::SetLine(std::size_t index, std::string value) {
    if (index >= lines_.size()) {
        lines_.resize(index + 1);
    }
    lines_[index] = std::move(value);
}

void MultiLineProgressRenderer::SetLines(std::vector<std::string> lines) {
    lines_ = std::move(lines);
}

const std::vector<std::string>& MultiLineProgressRenderer::Lines() const {
    return lines_;
}

void MultiLineProgressRenderer::Render(std::ostream& out) {
    if (has_rendered_ && rendered_line_count_ > 0) {
        out << "\x1b[" << rendered_line_count_ << "A";
    }

    RenderLinesAtCursor(out);
    last_render_at_ = std::chrono::steady_clock::now();
}

bool MultiLineProgressRenderer::RenderIfDue(
    std::ostream& out,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds cadence) {
    if (cadence.count() <= 0 || !has_rendered_ || now - last_render_at_ >= cadence) {
        if (has_rendered_ && rendered_line_count_ > 0) {
            out << "\x1b[" << rendered_line_count_ << "A";
        }
        RenderLinesAtCursor(out);
        last_render_at_ = now;
        return true;
    }
    return false;
}

void MultiLineProgressRenderer::WriteEventLine(std::ostream& out, const std::string& line) {
    WriteEventLines(out, std::vector<std::string>{ line });
}

void MultiLineProgressRenderer::WriteEventLines(std::ostream& out, const std::vector<std::string>& lines) {
    WriteEventLinesImpl(out, lines);
}

void MultiLineProgressRenderer::WriteEventLines(std::ostream& out, const std::deque<std::string>& lines) {
    WriteEventLinesImpl(out, lines);
}

template <typename TLineContainer>
void MultiLineProgressRenderer::WriteEventLinesImpl(std::ostream& out, const TLineContainer& event_lines) {
    if (event_lines.empty()) {
        return;
    }

    if (!has_rendered_ || rendered_line_count_ == 0) {
        for (const auto& line : event_lines) {
            out << line << '\n';
        }
        out.flush();
        return;
    }

    out << "\x1b[" << rendered_line_count_ << "A";
    out << "\x1b[0J";
    for (const auto& line : event_lines) {
        out << line << '\n';
    }
    RenderLinesAtCursor(out);
    last_render_at_ = std::chrono::steady_clock::now();
}

void MultiLineProgressRenderer::RenderLinesAtCursor(std::ostream& out) {
    const std::size_t lines_to_render = lines_.size() > rendered_line_count_ ? lines_.size() : rendered_line_count_;
    for (std::size_t i = 0; i < lines_to_render; ++i) {
        out << "\x1b[2K\r";
        if (i < lines_.size()) {
            out << lines_[i];
        }
        out << '\n';
    }
    out.flush();

    rendered_line_count_ = lines_.size();
    has_rendered_ = true;
}

} // namespace simcore::e2e
