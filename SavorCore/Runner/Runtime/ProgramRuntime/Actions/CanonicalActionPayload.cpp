#include "CanonicalActionPayload.h"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace savor::runtime::program {
namespace {

constexpr std::array<Byte, 4> kMagic{'S', 'A', 'P', '1'};
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kFieldHeaderSize = 8;

template <typename T>
void PutLittle(std::vector<Byte>& output, T value)
{
    static_assert(std::is_integral_v<T>);
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index)
    {
        output.push_back(static_cast<Byte>(
            bits >> (index * 8u)));
    }
}

template <typename T>
bool GetLittle(
    std::span<const Byte> input,
    std::size_t& cursor,
    T& value)
{
    static_assert(std::is_integral_v<T>);
    if (input.size() - cursor < sizeof(T))
        return false;
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned bits = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index)
    {
        bits |= static_cast<Unsigned>(input[cursor + index])
            << (index * 8u);
    }
    cursor += sizeof(T);
    value = static_cast<T>(bits);
    return true;
}

bool ValidUtf8(std::span<const Byte> bytes) noexcept
{
    std::size_t index = 0;
    while (index < bytes.size())
    {
        const Byte first = bytes[index++];
        if (first < 0x80u)
            continue;

        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if ((first & 0xe0u) == 0xc0u)
        {
            continuation = 1;
            codepoint = first & 0x1fu;
            if (codepoint < 2)
                return false;
        }
        else if ((first & 0xf0u) == 0xe0u)
        {
            continuation = 2;
            codepoint = first & 0x0fu;
        }
        else if ((first & 0xf8u) == 0xf0u)
        {
            continuation = 3;
            codepoint = first & 0x07u;
        }
        else
        {
            return false;
        }
        if (bytes.size() - index < continuation)
            return false;
        for (std::size_t offset = 0; offset < continuation; ++offset)
        {
            const Byte next = bytes[index++];
            if ((next & 0xc0u) != 0x80u)
                return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if ((continuation == 2 && codepoint < 0x800u) ||
            (continuation == 3 && codepoint < 0x10000u) ||
            codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu))
        {
            return false;
        }
    }
    return true;
}

CanonicalActionPayloadKind KindOf(
    const CanonicalActionPayloadValue& value) noexcept
{
    return std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>)
                return CanonicalActionPayloadKind::Boolean;
            else if constexpr (std::is_same_v<T, std::uint64_t>)
                return CanonicalActionPayloadKind::Unsigned;
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return CanonicalActionPayloadKind::Signed;
            else if constexpr (std::is_same_v<T, std::string>)
                return CanonicalActionPayloadKind::Utf8;
            else
                return CanonicalActionPayloadKind::Bytes;
        },
        value);
}

std::size_t PayloadSize(
    const CanonicalActionPayloadValue& value) noexcept
{
    return std::visit(
        [](const auto& item) -> std::size_t {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>)
                return 1;
            else if constexpr (
                std::is_same_v<T, std::uint64_t> ||
                std::is_same_v<T, std::int64_t>)
            {
                return 8;
            }
            else
            {
                return item.size();
            }
        },
        value);
}

const ProgramValue* RootValue(const ProgramValueGraph& graph)
{
    const auto found = std::ranges::find(
        graph.values,
        graph.root,
        &ProgramValue::id);
    return found == graph.values.end() ? nullptr : &*found;
}

void SetDiagnostic(std::string* output, std::string value)
{
    if (output)
        *output = std::move(value);
}

} // namespace

bool CanonicalActionPayload::AddBoolean(
    CanonicalActionPayloadField field,
    bool value)
{
    return Add({field, value});
}

bool CanonicalActionPayload::AddUnsigned(
    CanonicalActionPayloadField field,
    std::uint64_t value)
{
    return Add({field, value});
}

bool CanonicalActionPayload::AddSigned(
    CanonicalActionPayloadField field,
    std::int64_t value)
{
    return Add({field, value});
}

bool CanonicalActionPayload::AddUtf8(
    CanonicalActionPayloadField field,
    std::string value)
{
    return Add({field, std::move(value)});
}

bool CanonicalActionPayload::AddBytes(
    CanonicalActionPayloadField field,
    std::vector<Byte> value)
{
    return Add({field, std::move(value)});
}

std::optional<bool> CanonicalActionPayload::Boolean(
    CanonicalActionPayloadField field) const
{
    const auto* entry = Find(field);
    const auto* value = entry
        ? std::get_if<bool>(&entry->value)
        : nullptr;
    return value ? std::optional<bool>(*value) : std::nullopt;
}

std::optional<std::uint64_t> CanonicalActionPayload::Unsigned(
    CanonicalActionPayloadField field) const
{
    const auto* entry = Find(field);
    const auto* value = entry
        ? std::get_if<std::uint64_t>(&entry->value)
        : nullptr;
    return value ? std::optional<std::uint64_t>(*value)
                 : std::nullopt;
}

std::optional<std::int64_t> CanonicalActionPayload::Signed(
    CanonicalActionPayloadField field) const
{
    const auto* entry = Find(field);
    const auto* value = entry
        ? std::get_if<std::int64_t>(&entry->value)
        : nullptr;
    return value ? std::optional<std::int64_t>(*value)
                 : std::nullopt;
}

std::optional<std::string_view> CanonicalActionPayload::Utf8(
    CanonicalActionPayloadField field) const
{
    const auto* entry = Find(field);
    const auto* value = entry
        ? std::get_if<std::string>(&entry->value)
        : nullptr;
    return value ? std::optional<std::string_view>(*value)
                 : std::nullopt;
}

std::optional<std::span<const Byte>> CanonicalActionPayload::Bytes(
    CanonicalActionPayloadField field) const
{
    const auto* entry = Find(field);
    const auto* value = entry
        ? std::get_if<std::vector<Byte>>(&entry->value)
        : nullptr;
    return value
        ? std::optional<std::span<const Byte>>(
              std::span<const Byte>(*value))
        : std::nullopt;
}

bool CanonicalActionPayload::Contains(
    CanonicalActionPayloadField field) const noexcept
{
    return Find(field) != nullptr;
}

bool CanonicalActionPayload::Add(CanonicalActionPayloadEntry entry)
{
    const auto found = std::ranges::lower_bound(
        entries_,
        entry.field,
        {},
        &CanonicalActionPayloadEntry::field);
    if (found != entries_.end() && found->field == entry.field)
        return false;
    entries_.insert(found, std::move(entry));
    return true;
}

const CanonicalActionPayloadEntry* CanonicalActionPayload::Find(
    CanonicalActionPayloadField field) const noexcept
{
    const auto found = std::ranges::lower_bound(
        entries_,
        field,
        {},
        &CanonicalActionPayloadEntry::field);
    return found != entries_.end() && found->field == field
        ? &*found
        : nullptr;
}

CanonicalActionPayloadResult EncodeCanonicalActionPayload(
    const CanonicalActionPayload& payload,
    const SchemaIdentity& nominal_schema,
    CanonicalActionPayloadLimits limits)
{
    if (nominal_schema.canonical_id.empty() ||
        nominal_schema.version == 0 ||
        nominal_schema.schema_hash.empty())
    {
        return {
            false,
            {},
            "Canonical action payload requires an exact nominal schema"};
    }
    if (limits.maximum_encoded_bytes == 0 ||
        limits.maximum_encoded_bytes > 64u * 1024u ||
        payload.entries().size() > limits.maximum_fields ||
        payload.entries().size() >
            std::numeric_limits<std::uint16_t>::max())
    {
        return {
            false,
            {},
            "Canonical action payload exceeds its field or envelope bound"};
    }

    std::size_t encoded_size = kHeaderSize;
    for (const auto& entry : payload.entries())
    {
        const std::size_t size = PayloadSize(entry.value);
        if (size > std::numeric_limits<std::uint32_t>::max())
        {
            return {false, {}, "Canonical action field is too large"};
        }
        if (std::holds_alternative<std::string>(entry.value))
        {
            const auto& text = std::get<std::string>(entry.value);
            const auto bytes = std::span<const Byte>(
                reinterpret_cast<const Byte*>(text.data()),
                text.size());
            if (size > limits.maximum_text_bytes || !ValidUtf8(bytes))
            {
                return {
                    false,
                    {},
                    "Canonical action text is invalid or over its bound"};
            }
        }
        if (std::holds_alternative<std::vector<Byte>>(entry.value) &&
            size > limits.maximum_blob_bytes)
        {
            return {
                false,
                {},
                "Canonical action byte field exceeds its bound"};
        }
        if (encoded_size > limits.maximum_encoded_bytes -
                std::min(
                    limits.maximum_encoded_bytes,
                    kFieldHeaderSize + size))
        {
            return {
                false,
                {},
                "Canonical action payload exceeds its encoded bound"};
        }
        encoded_size += kFieldHeaderSize + size;
    }

    std::vector<Byte> encoded;
    encoded.reserve(encoded_size);
    encoded.insert(encoded.end(), kMagic.begin(), kMagic.end());
    PutLittle(encoded, kVersion);
    PutLittle(
        encoded,
        static_cast<std::uint16_t>(payload.entries().size()));
    for (const auto& entry : payload.entries())
    {
        PutLittle(
            encoded,
            static_cast<std::uint16_t>(entry.field));
        encoded.push_back(static_cast<Byte>(KindOf(entry.value)));
        encoded.push_back(0);
        PutLittle(
            encoded,
            static_cast<std::uint32_t>(PayloadSize(entry.value)));
        std::visit(
            [&encoded](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, bool>)
                {
                    encoded.push_back(item ? 1 : 0);
                }
                else if constexpr (
                    std::is_same_v<T, std::uint64_t> ||
                    std::is_same_v<T, std::int64_t>)
                {
                    PutLittle(encoded, item);
                }
                else
                {
                    encoded.insert(
                        encoded.end(),
                        reinterpret_cast<const Byte*>(item.data()),
                        reinterpret_cast<const Byte*>(item.data()) +
                            item.size());
                }
            },
            entry.value);
    }

    ProgramValue value;
    value.id = ProgramValueId(1);
    value.type = TypeRef::Named(nominal_schema);
    value.payload = std::move(encoded);
    return {
        true,
        ProgramValueGraph{value.id, {std::move(value)}},
        {}};
}

bool DecodeCanonicalActionPayload(
    const ProgramValueGraph& graph,
    const SchemaIdentity& expected_schema,
    CanonicalActionPayload& payload,
    std::string* diagnostic,
    CanonicalActionPayloadLimits limits)
{
    if (graph.values.size() != 1)
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action envelope must contain exactly one value");
        return false;
    }
    const ProgramValue* root = RootValue(graph);
    const auto* bytes = root
        ? std::get_if<std::vector<Byte>>(&root->payload)
        : nullptr;
    if (!root || !root->type.named ||
        *root->type.named != expected_schema ||
        !bytes)
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action payload does not match its expected "
            "nominal schema");
        return false;
    }
    if (bytes->size() < kHeaderSize ||
        bytes->size() > limits.maximum_encoded_bytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes->begin()))
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action envelope has invalid magic or size");
        return false;
    }

    const std::span<const Byte> input(*bytes);
    std::size_t cursor = kMagic.size();
    std::uint16_t version = 0;
    std::uint16_t field_count = 0;
    if (!GetLittle(input, cursor, version) ||
        !GetLittle(input, cursor, field_count) ||
        version != kVersion ||
        field_count > limits.maximum_fields)
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action envelope version or field count is invalid");
        return false;
    }

    CanonicalActionPayload candidate;
    std::uint16_t prior_field = 0;
    for (std::uint16_t index = 0; index < field_count; ++index)
    {
        std::uint16_t raw_field = 0;
        std::uint8_t raw_kind = 0;
        std::uint8_t reserved = 0;
        std::uint32_t size = 0;
        if (!GetLittle(input, cursor, raw_field) ||
            !GetLittle(input, cursor, raw_kind) ||
            !GetLittle(input, cursor, reserved) ||
            !GetLittle(input, cursor, size) ||
            raw_field == 0 ||
            (index != 0 && raw_field <= prior_field) ||
            reserved != 0 ||
            size > input.size() - cursor)
        {
            SetDiagnostic(
                diagnostic,
                "Canonical action field header is malformed or non-canonical");
            return false;
        }
        prior_field = raw_field;
        const auto field =
            static_cast<CanonicalActionPayloadField>(raw_field);
        const auto kind =
            static_cast<CanonicalActionPayloadKind>(raw_kind);
        const std::span<const Byte> value =
            input.subspan(cursor, size);
        cursor += size;

        bool added = false;
        switch (kind)
        {
        case CanonicalActionPayloadKind::Boolean:
            added = size == 1 && value[0] <= 1 &&
                candidate.AddBoolean(field, value[0] != 0);
            break;
        case CanonicalActionPayloadKind::Unsigned:
        {
            std::size_t value_cursor = 0;
            std::uint64_t decoded = 0;
            added = size == 8 &&
                GetLittle(value, value_cursor, decoded) &&
                candidate.AddUnsigned(field, decoded);
            break;
        }
        case CanonicalActionPayloadKind::Signed:
        {
            std::size_t value_cursor = 0;
            std::int64_t decoded = 0;
            added = size == 8 &&
                GetLittle(value, value_cursor, decoded) &&
                candidate.AddSigned(field, decoded);
            break;
        }
        case CanonicalActionPayloadKind::Utf8:
            added = size <= limits.maximum_text_bytes &&
                ValidUtf8(value) &&
                candidate.AddUtf8(
                    field,
                    std::string(
                        reinterpret_cast<const char*>(value.data()),
                        value.size()));
            break;
        case CanonicalActionPayloadKind::Bytes:
            added = size <= limits.maximum_blob_bytes &&
                candidate.AddBytes(
                    field,
                    std::vector<Byte>(value.begin(), value.end()));
            break;
        default:
            break;
        }
        if (!added)
        {
            SetDiagnostic(
                diagnostic,
                "Canonical action field value is invalid");
            return false;
        }
    }
    if (cursor != input.size())
    {
        SetDiagnostic(
            diagnostic,
            "Canonical action envelope has trailing bytes");
        return false;
    }
    payload = std::move(candidate);
    return true;
}

} // namespace savor::runtime::program
