#include "TasMovieInputEpochModule.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace savor::runtime::tasmovie::inputepoch {
namespace {

void SetDiagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

bool IsSha256(std::string_view value)
{
    return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

class Writer
{
public:
    explicit Writer(std::array<char, 4> magic)
    {
        for (const char ch : magic)
            bytes_.push_back(static_cast<std::uint8_t>(ch));
    }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void U64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift != 64; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void Text(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Frame(const GCInputFrame& frame)
    {
        bytes_.push_back(static_cast<std::uint8_t>(frame.buttons));
        bytes_.push_back(static_cast<std::uint8_t>(frame.buttons >> 8u));
        bytes_.push_back(frame.main_x);
        bytes_.push_back(frame.main_y);
        bytes_.push_back(frame.c_x);
        bytes_.push_back(frame.c_y);
        bytes_.push_back(frame.trig_l);
        bytes_.push_back(frame.trig_r);
    }
    void Bytes(std::span<const std::uint8_t> value)
    {
        U64(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> Finish() && { return std::move(bytes_); }
private:
    std::vector<std::uint8_t> bytes_;
};

class Reader
{
public:
    Reader(std::span<const std::uint8_t> bytes, std::array<char, 4> magic)
        : bytes_(bytes)
    {
        valid_ = bytes_.size() >= 4;
        for (std::size_t index = 0; valid_ && index != 4; ++index)
            valid_ = bytes_[index] == static_cast<std::uint8_t>(magic[index]);
        offset_ = valid_ ? 4 : bytes_.size();
    }
    bool U32(std::uint32_t& value)
    {
        if (!Take(4)) return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        if (!Take(8)) return false;
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool Text(std::string& value)
    {
        std::uint32_t size = 0;
        if (!U32(size) || !Take(size)) return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    bool Frame(GCInputFrame& frame)
    {
        if (!Take(8)) return false;
        frame.buttons = static_cast<std::uint16_t>(bytes_[offset_]) |
            (static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8u);
        frame.main_x = bytes_[offset_ + 2];
        frame.main_y = bytes_[offset_ + 3];
        frame.c_x = bytes_[offset_ + 4];
        frame.c_y = bytes_[offset_ + 5];
        frame.trig_l = bytes_[offset_ + 6];
        frame.trig_r = bytes_[offset_ + 7];
        offset_ += 8;
        return true;
    }
    bool Bytes(std::vector<std::uint8_t>& value)
    {
        std::uint64_t size = 0;
        if (!U64(size) || size > std::numeric_limits<std::size_t>::max() ||
            !Take(static_cast<std::size_t>(size))) return false;
        value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += static_cast<std::size_t>(size);
        return true;
    }
    bool Complete() const noexcept { return valid_ && offset_ == bytes_.size(); }
private:
    bool Take(std::size_t count)
    {
        if (!valid_ || count > bytes_.size() - offset_)
        {
            valid_ = false;
            return false;
        }
        return true;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = false;
};

bool ValidateRequestPath(std::string_view path)
{
    return !path.empty() && path.size() <= 4096;
}

} // namespace

GCInputFrame DecodeGuestPadStatusV1(std::uint64_t packed_status) noexcept
{
    const auto byte = [packed_status](std::size_t index) {
        return static_cast<std::uint8_t>(
            packed_status >> ((7u - index) * 8u));
    };
    const auto stick = [&byte](std::size_t index) {
        return SaturateStickToU8(
            static_cast<int>(static_cast<std::int8_t>(byte(index))));
    };

    GCInputFrame frame{};
    frame.buttons = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte(0)) << 8u) | byte(1));
    frame.main_x = stick(2);
    frame.main_y = stick(3);
    frame.c_x = stick(4);
    frame.c_y = stick(5);
    frame.trig_l = byte(6);
    frame.trig_r = byte(7);
    return frame;
}

bool ValidateInputEpochScheduleV1(
    const TasMovieInputEpochScheduleV1& schedule,
    std::string* diagnostic)
{
    if (!IsSha256(schedule.source_dtm_sha256) ||
        schedule.source_poll_count == 0 || schedule.epochs.empty() ||
        schedule.epochs.size() > MaximumEpochs)
    {
        SetDiagnostic(diagnostic, "input-epoch schedule header is invalid");
        return false;
    }
    std::uint64_t prior_cursor = 0;
    for (const auto& epoch : schedule.epochs)
    {
        if (epoch.movie_input_cursor == 0 ||
            epoch.movie_input_cursor > schedule.source_poll_count ||
            epoch.movie_input_cursor < prior_cursor)
        {
            SetDiagnostic(diagnostic, "input-epoch schedule cursors are invalid");
            return false;
        }
        prior_cursor = epoch.movie_input_cursor;
    }
    if (diagnostic) diagnostic->clear();
    return true;
}

std::vector<std::uint8_t> EncodeInputEpochScheduleArtifactV1(
    const TasMovieInputEpochScheduleV1& schedule,
    std::string* diagnostic)
{
    if (!ValidateInputEpochScheduleV1(schedule, diagnostic)) return {};
    Writer writer({'T','E','S','1'});
    writer.U32(ContractVersion);
    writer.Text(schedule.source_dtm_sha256);
    writer.U64(schedule.source_poll_count);
    writer.U64(schedule.epochs.size());
    for (const auto& epoch : schedule.epochs)
    {
        writer.U64(epoch.movie_input_cursor);
        writer.Frame(epoch.input);
    }
    return std::move(writer).Finish();
}

bool DecodeInputEpochScheduleArtifactV1(
    std::span<const std::uint8_t> bytes,
    TasMovieInputEpochScheduleV1& schedule,
    std::string* diagnostic)
{
    Reader reader(bytes, {'T','E','S','1'});
    std::uint32_t version = 0;
    std::uint64_t count = 0;
    TasMovieInputEpochScheduleV1 decoded;
    if (!reader.U32(version) || version != ContractVersion ||
        !reader.Text(decoded.source_dtm_sha256) ||
        !reader.U64(decoded.source_poll_count) || !reader.U64(count) ||
        count == 0 || count > MaximumEpochs)
    {
        SetDiagnostic(diagnostic, "input-epoch schedule payload is malformed");
        return false;
    }
    decoded.epochs.resize(static_cast<std::size_t>(count));
    for (auto& epoch : decoded.epochs)
    {
        if (!reader.U64(epoch.movie_input_cursor) || !reader.Frame(epoch.input))
        {
            SetDiagnostic(diagnostic, "input-epoch schedule entry is malformed");
            return false;
        }
    }
    if (!reader.Complete() || !ValidateInputEpochScheduleV1(decoded, diagnostic))
        return false;
    schedule = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> EncodeAnnotationExecutionInputV1(
    const TasMovieInputEpochAnnotationRequestV1& request,
    std::string* diagnostic)
{
    if (!ValidateRequestPath(request.source_dtm_path) ||
        !IsSha256(request.source_dtm_sha256) || request.source_poll_count == 0 ||
        request.source_poll_count > MaximumEpochs)
    {
        SetDiagnostic(diagnostic, "annotation execution input is invalid");
        return {};
    }
    Writer writer({'T','E','A','2'});
    writer.U32(ContractVersion);
    writer.Text(request.source_dtm_path);
    writer.Text(request.source_dtm_sha256);
    writer.U64(request.source_poll_count);
    return std::move(writer).Finish();
}

bool DecodeAnnotationExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    TasMovieInputEpochAnnotationRequestV1& request,
    std::string* diagnostic)
{
    Reader reader(bytes, {'T','E','A','2'});
    std::uint32_t version = 0;
    std::uint64_t count = 0;
    TasMovieInputEpochAnnotationRequestV1 decoded;
    if (!reader.U32(version) || version != ContractVersion ||
        !reader.Text(decoded.source_dtm_path) ||
        !reader.Text(decoded.source_dtm_sha256) || !reader.U64(count) ||
        count == 0 || count > MaximumEpochs)
    {
        SetDiagnostic(diagnostic, "annotation execution payload is malformed");
        return false;
    }
    decoded.source_poll_count = count;
    if (!reader.Complete() || !ValidateRequestPath(decoded.source_dtm_path) ||
        !IsSha256(decoded.source_dtm_sha256)) return false;
    request = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> EncodeRewriteExecutionInputV1(
    const TasMovieInputEpochRewriteRequestV1& request,
    std::string* diagnostic)
{
    const auto schedule = EncodeInputEpochScheduleArtifactV1(
        request.schedule, diagnostic);
    if (schedule.empty() ||
        request.insert_before_epoch >= request.schedule.epochs.size() ||
        request.neutral_epoch_count == 0 ||
        !ValidateRequestPath(request.source_dtm_path) ||
        !ValidateRequestPath(request.output_dtm_path) ||
        !ValidateRequestPath(request.output_savestate_path))
    {
        SetDiagnostic(diagnostic, "rewrite execution input is invalid");
        return {};
    }
    Writer writer({'T','E','R','1'});
    writer.U32(ContractVersion);
    writer.Text(request.source_dtm_path);
    writer.Bytes(schedule);
    writer.U64(request.insert_before_epoch);
    writer.U64(request.neutral_epoch_count);
    writer.Text(request.output_dtm_path);
    writer.Text(request.output_savestate_path);
    return std::move(writer).Finish();
}

bool DecodeRewriteExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    TasMovieInputEpochRewriteRequestV1& request,
    std::string* diagnostic)
{
    Reader reader(bytes, {'T','E','R','1'});
    std::uint32_t version = 0;
    std::vector<std::uint8_t> schedule;
    TasMovieInputEpochRewriteRequestV1 decoded;
    if (!reader.U32(version) || version != ContractVersion ||
        !reader.Text(decoded.source_dtm_path) || !reader.Bytes(schedule) ||
        !reader.U64(decoded.insert_before_epoch) ||
        !reader.U64(decoded.neutral_epoch_count) ||
        !reader.Text(decoded.output_dtm_path) ||
        !reader.Text(decoded.output_savestate_path) || !reader.Complete() ||
        !DecodeInputEpochScheduleArtifactV1(
            schedule, decoded.schedule, diagnostic) ||
        decoded.insert_before_epoch >= decoded.schedule.epochs.size() ||
        decoded.neutral_epoch_count == 0 ||
        !ValidateRequestPath(decoded.source_dtm_path) ||
        !ValidateRequestPath(decoded.output_dtm_path) ||
        !ValidateRequestPath(decoded.output_savestate_path)) return false;
    request = std::move(decoded);
    return true;
}

} // namespace savor::runtime::tasmovie::inputepoch
