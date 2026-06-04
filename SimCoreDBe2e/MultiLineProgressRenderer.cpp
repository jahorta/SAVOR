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
}

void MultiLineProgressRenderer::WriteEventLine(std::ostream& out, const std::string& line) {
    if (!has_rendered_ || rendered_line_count_ == 0) {
        out << line << '\n';
        out.flush();
        return;
    }

    out << "\x1b[" << rendered_line_count_ << "A";
    out << "\x1b[0J";
    out << line << '\n';
    RenderLinesAtCursor(out);
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
