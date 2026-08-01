#pragma once

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace savor::runtime {

struct RuntimeArtifactReservation
{
    std::string logical_label;
    std::filesystem::path output_path;
};

// Host-owned mapping from a program's logical artifact label to physical
// storage. Programs never supply filesystem paths.
class RuntimeArtifactSink final
{
public:
    explicit RuntimeArtifactSink(std::filesystem::path root = {})
        : root_(std::move(root))
    {
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] std::optional<RuntimeArtifactReservation> Reserve(
        std::string_view logical_label,
        std::string_view extension,
        std::string* error_out = nullptr)
    {
        const auto fail = [&](std::string message)
            -> std::optional<RuntimeArtifactReservation> {
            if (error_out != nullptr)
                *error_out = std::move(message);
            return std::nullopt;
        };
        if (root_.empty())
            return fail("runtime artifact sink has no host storage root");
        if (logical_label.empty() || logical_label.size() > 128)
            return fail("runtime artifact label is empty or too long");
        for (const unsigned char character : logical_label)
        {
            if (!std::isalnum(character) && character != '-'
                && character != '_' && character != '.')
            {
                return fail("runtime artifact label contains an unsafe character");
            }
        }
        if (extension.empty() || extension.front() != '.'
            || extension.find_first_of("/\\") != std::string_view::npos)
        {
            return fail("runtime artifact extension is invalid");
        }
        std::error_code filesystem_error;
        std::filesystem::create_directories(root_, filesystem_error);
        if (filesystem_error)
            return fail("runtime artifact root could not be created: "
                + filesystem_error.message());

        const auto sequence = next_sequence_++;
        RuntimeArtifactReservation reservation{
            .logical_label = std::string(logical_label),
            .output_path = root_ /
                (std::string(logical_label) + "-"
                    + std::to_string(sequence) + std::string(extension)),
        };
        if (error_out != nullptr)
            error_out->clear();
        return reservation;
    }

private:
    std::filesystem::path root_;
    std::uint64_t next_sequence_ = 1;
};

} // namespace savor::runtime
