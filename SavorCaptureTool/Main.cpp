#include "../SavorCaptureFormat/CaptureFormat.h"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

void usage()
{
    std::cerr
        << "Usage:\n"
        << "  SavorCaptureTool info CAPTURE.scap\n"
        << "  SavorCaptureTool verify CAPTURE.scap\n"
        << "  SavorCaptureTool query CAPTURE.scap [--probe ID] [--from N] [--to N] [--raw-bytes]\n"
        << "  SavorCaptureTool export CAPTURE.scap --output FILE.jsonl [--probe ID] [--from N] [--to N] [--raw-bytes]\n"
        << "  SavorCaptureTool recover CAPTURE.scap --output RECOVERED.scap\n";
}

bool parse_u64(std::string_view text, std::uint64_t& value)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

struct QueryOptions {
    std::optional<std::string> probe;
    std::uint64_t first = 0;
    std::uint64_t last = UINT64_MAX;
    std::filesystem::path output;
    bool raw_bytes = false;
};

bool parse_options(int argc, char** argv, int first_arg, QueryOptions& options)
{
    for (int i = first_arg; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--probe" && i + 1 < argc) {
            options.probe = argv[++i];
        } else if (arg == "--from" && i + 1 < argc) {
            if (!parse_u64(argv[++i], options.first))
                return false;
        } else if (arg == "--to" && i + 1 < argc) {
            if (!parse_u64(argv[++i], options.last))
                return false;
        } else if (arg == "--output" && i + 1 < argc) {
            options.output = argv[++i];
        } else if (arg == "--raw-bytes") {
            options.raw_bytes = true;
        } else {
            return false;
        }
    }
    return options.first <= options.last;
}

void print_report(const savor::capture_format::VerificationReport& report)
{
    std::cout << "ok=" << (report.ok ? "true" : "false")
        << " recoverable=" << (report.recoverable ? "true" : "false")
        << " segments=" << report.segments.size() << '\n';
    for (const auto& segment : report.segments) {
        std::cout << "segment=" << segment.path.string()
            << " events=" << segment.event_count
            << " chunks=" << segment.chunks.size()
            << " footer=" << (segment.has_footer ? "true" : "false")
            << " complete=" << (segment.complete ? "true" : "false")
            << " source=" << segment.metadata.source_identity << '\n';
    }
    for (const auto& warning : report.warnings)
        std::cout << "warning=" << warning << '\n';
    for (const auto& error : report.errors)
        std::cout << "error=" << error << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    const std::filesystem::path input = argv[2];

    if (command == "info" || command == "verify") {
        const auto report = savor::capture_format::Reader::verify(input);
        print_report(report);
        return report.ok ? 0 : 1;
    }

    QueryOptions options;
    if (!parse_options(argc, argv, 3, options)) {
        usage();
        return 2;
    }

    if (command == "query") {
        std::vector<savor::capture_format::Event> events;
        std::string error;
        if (!savor::capture_format::Reader::read_all(input, events, nullptr, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        for (const auto& event : events) {
            if (event.capture_sequence < options.first || event.capture_sequence > options.last)
                continue;
            if (options.probe.has_value() && event.probe_id != *options.probe)
                continue;
            std::cout << savor::capture_format::event_to_json(event, options.raw_bytes) << '\n';
        }
        return 0;
    }

    if (command == "export") {
        if (options.output.empty()) {
            usage();
            return 2;
        }
        std::string error;
        const std::optional<std::string_view> probe = options.probe.has_value()
            ? std::optional<std::string_view>(*options.probe)
            : std::nullopt;
        if (!savor::capture_format::Reader::export_jsonl(
            input, options.output, probe, options.first, options.last,
            options.raw_bytes, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        return 0;
    }

    if (command == "recover") {
        if (options.output.empty()) {
            usage();
            return 2;
        }
        std::string error;
        if (!savor::capture_format::Reader::recover(input, options.output, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        return 0;
    }

    usage();
    return 2;
}
