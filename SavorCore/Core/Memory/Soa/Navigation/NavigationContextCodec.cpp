#include "NavigationContextCodec.h"

#include <bit>
#include <cstring>
#include <utility>

namespace soa::navigation::ctx::codec {
namespace {

constexpr char Magic[] = {'N', 'C', 'T', 'X'};
constexpr std::uint16_t KnownFlags = FlagGroundPresent;
static_assert(sizeof(float) == sizeof(std::uint32_t));

bool IsMem1Range(std::uint32_t address, std::size_t size)
{
    if (address < savor::MemView::kMem1Base) return false;
    const auto offset = static_cast<std::uint64_t>(address)
        - savor::MemView::kMem1Base;
    return offset + size <= savor::MemView::kMem1Size;
}

void SetFailure(ExtractFailure value, ExtractFailure* out)
{
    if (out != nullptr) *out = value;
}

bool ReadF32(const savor::MemView& view, std::uint32_t address, float& out)
{
    std::uint32_t bits = 0;
    if (!view.read_u32(address, bits)) return false;
    out = std::bit_cast<float>(bits);
    return true;
}

void PutU16(std::string& out, std::uint16_t value)
{
    out.push_back(static_cast<char>(value));
    out.push_back(static_cast<char>(value >> 8));
}

void PutU32(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>(value));
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value >> 16));
    out.push_back(static_cast<char>(value >> 24));
}

void PutF32(std::string& out, float value)
{
    PutU32(out, std::bit_cast<std::uint32_t>(value));
}

class Cursor {
public:
    explicit Cursor(std::string_view input)
        : cursor_(reinterpret_cast<const std::uint8_t*>(input.data())),
          end_(cursor_ + input.size())
    {
    }

    bool GetU8(std::uint8_t& value)
    {
        if (Remaining() < 1) return false;
        value = *cursor_++;
        return true;
    }

    bool GetU16(std::uint16_t& value)
    {
        if (Remaining() < 2) return false;
        value = static_cast<std::uint16_t>(cursor_[0])
            | (static_cast<std::uint16_t>(cursor_[1]) << 8);
        cursor_ += 2;
        return true;
    }

    bool GetU32(std::uint32_t& value)
    {
        if (Remaining() < 4) return false;
        value = static_cast<std::uint32_t>(cursor_[0])
            | (static_cast<std::uint32_t>(cursor_[1]) << 8)
            | (static_cast<std::uint32_t>(cursor_[2]) << 16)
            | (static_cast<std::uint32_t>(cursor_[3]) << 24);
        cursor_ += 4;
        return true;
    }

    bool GetF32(float& value)
    {
        std::uint32_t bits = 0;
        if (!GetU32(bits)) return false;
        value = std::bit_cast<float>(bits);
        return true;
    }

    bool GetBytes(void* output, std::size_t size)
    {
        if (Remaining() < size) return false;
        std::memcpy(output, cursor_, size);
        cursor_ += size;
        return true;
    }

    std::size_t Remaining() const
    {
        return static_cast<std::size_t>(end_ - cursor_);
    }

private:
    const std::uint8_t* cursor_;
    const std::uint8_t* end_;
};

bool IsValidModel(const NavigationContext& value)
{
    if (value.capture_pc != CapturePc
        || !IsMem1Range(value.player_worksheet, 0x1bcu)
        || value.motion_state != 1u) {
        return false;
    }
    return value.has_ground || value.ground_tbl_id == 0u;
}

} // namespace

bool extract_from_mem1(
    const savor::MemView& view,
    std::uint32_t capture_pc,
    NavigationContext& out,
    ExtractFailure* failure_out)
{
    SetFailure(ExtractFailure::None, failure_out);
    if (!view.valid()) {
        SetFailure(ExtractFailure::InvalidMem1, failure_out);
        return false;
    }
    if (capture_pc != CapturePc) {
        SetFailure(ExtractFailure::CapturePcMismatch, failure_out);
        return false;
    }

    NavigationContext value{};
    value.capture_pc = capture_pc;
    if (!view.read_u32(PlayerWorksheetPointerAddress, value.player_worksheet)
        || !view.read_u32(AreaAddress, value.area)
        || !view.read_u8(SubareaAddress, value.subarea)
        || !view.read_u32(
            PostInputMovementSuppressAddress,
            value.post_input_movement_suppress)
        || !ReadF32(
            view,
            StepDistanceCarryInAddress,
            value.step_distance_carry_in)) {
        SetFailure(ExtractFailure::InvalidMem1, failure_out);
        return false;
    }

    const auto worksheet = value.player_worksheet;
    std::uint32_t ground_selector_pointer = 0;
    if (worksheet == 0
        || !IsMem1Range(worksheet, 0x1bcu)
        || !view.read_u16(worksheet + 0x170u, value.motion_state)
        || !view.read_u16(worksheet + 0x172u, value.motion_substate)
        || !ReadF32(view, worksheet + 0x38u, value.position_x)
        || !ReadF32(view, worksheet + 0x3cu, value.position_y)
        || !ReadF32(view, worksheet + 0x40u, value.position_z)
        || !view.read_u32(worksheet + 0x44u, value.rotation_x_raw)
        || !view.read_u32(worksheet + 0x48u, value.rotation_y_raw)
        || !view.read_u32(worksheet + 0x4cu, value.rotation_z_raw)
        || !ReadF32(
            view,
            worksheet + 0x104u,
            value.previous_position_x)
        || !ReadF32(
            view,
            worksheet + 0x108u,
            value.previous_position_y)
        || !ReadF32(
            view,
            worksheet + 0x10cu,
            value.previous_position_z)
        || !view.read_u32(
            worksheet + 0x110u,
            value.previous_rotation_x_raw)
        || !view.read_u32(
            worksheet + 0x114u,
            value.previous_rotation_y_raw)
        || !view.read_u32(
            worksheet + 0x118u,
            value.previous_rotation_z_raw)
        || !view.read_u32(
            worksheet + GroundSelectorPointerOffset,
            ground_selector_pointer)) {
        SetFailure(ExtractFailure::InvalidWorksheet, failure_out);
        return false;
    }

    if (value.motion_state != 1u) {
        SetFailure(ExtractFailure::MotionStateMismatch, failure_out);
        return false;
    }

    if (ground_selector_pointer != 0) {
        // Preserve the capture contract's full-record readability check even
        // though the compact artifact retains only the committed entry TBLID.
        if (!IsMem1Range(
                ground_selector_pointer,
                GroundSelectorRecordSize)
            || !view.read_u16(
                ground_selector_pointer + GroundTblIdOffset,
                value.ground_tbl_id)) {
            SetFailure(ExtractFailure::InvalidGroundSelector, failure_out);
            return false;
        }
        value.has_ground = true;
    }

    out = std::move(value);
    return true;
}

bool encode(const NavigationContext& in, std::string& out)
{
    out.clear();
    if (!IsValidModel(in)) return false;

    const bool has_ground = in.has_ground;
    std::string encoded;
    encoded.reserve(EncodedSize);
    encoded.append(Magic, sizeof(Magic));
    PutU16(encoded, Version);
    PutU16(encoded, has_ground ? FlagGroundPresent : 0u);
    PutU32(encoded, in.capture_pc);
    PutU32(encoded, in.player_worksheet);
    PutU32(encoded, in.area);
    encoded.push_back(static_cast<char>(in.subarea));
    encoded.append(3, '\0');
    PutU16(encoded, in.motion_state);
    PutU16(encoded, in.motion_substate);
    PutU32(encoded, in.post_input_movement_suppress);
    PutF32(encoded, in.position_x);
    PutF32(encoded, in.position_y);
    PutF32(encoded, in.position_z);
    PutU32(encoded, in.rotation_x_raw);
    PutU32(encoded, in.rotation_y_raw);
    PutU32(encoded, in.rotation_z_raw);
    PutF32(encoded, in.previous_position_x);
    PutF32(encoded, in.previous_position_y);
    PutF32(encoded, in.previous_position_z);
    PutU32(encoded, in.previous_rotation_x_raw);
    PutU32(encoded, in.previous_rotation_y_raw);
    PutU32(encoded, in.previous_rotation_z_raw);
    PutF32(encoded, in.step_distance_carry_in);
    PutU16(encoded, in.ground_tbl_id);

    if (encoded.size() != EncodedSize) return false;
    out = std::move(encoded);
    return true;
}

bool decode(std::string_view in, NavigationContext& out)
{
    if (in.size() != EncodedSize) {
        return false;
    }

    Cursor cursor(in);
    char magic[sizeof(Magic)]{};
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    NavigationContext value{};
    if (!cursor.GetBytes(magic, sizeof(magic))
        || std::memcmp(magic, Magic, sizeof(Magic)) != 0
        || !cursor.GetU16(version)
        || version != Version
        || !cursor.GetU16(flags)
        || (flags & ~KnownFlags) != 0) {
        return false;
    }

    std::uint8_t reserved[3]{};
    if (!cursor.GetU32(value.capture_pc)
        || !cursor.GetU32(value.player_worksheet)
        || !cursor.GetU32(value.area)
        || !cursor.GetU8(value.subarea)
        || !cursor.GetBytes(reserved, sizeof(reserved))
        || reserved[0] != 0 || reserved[1] != 0 || reserved[2] != 0
        || !cursor.GetU16(value.motion_state)
        || !cursor.GetU16(value.motion_substate)
        || !cursor.GetU32(value.post_input_movement_suppress)
        || !cursor.GetF32(value.position_x)
        || !cursor.GetF32(value.position_y)
        || !cursor.GetF32(value.position_z)
        || !cursor.GetU32(value.rotation_x_raw)
        || !cursor.GetU32(value.rotation_y_raw)
        || !cursor.GetU32(value.rotation_z_raw)
        || !cursor.GetF32(value.previous_position_x)
        || !cursor.GetF32(value.previous_position_y)
        || !cursor.GetF32(value.previous_position_z)
        || !cursor.GetU32(value.previous_rotation_x_raw)
        || !cursor.GetU32(value.previous_rotation_y_raw)
        || !cursor.GetU32(value.previous_rotation_z_raw)
        || !cursor.GetF32(value.step_distance_carry_in)
        || !cursor.GetU16(value.ground_tbl_id)) {
        return false;
    }

    value.has_ground = (flags & FlagGroundPresent) != 0;
    if (cursor.Remaining() != 0 || !IsValidModel(value)) return false;

    out = std::move(value);
    return true;
}

} // namespace soa::navigation::ctx::codec
