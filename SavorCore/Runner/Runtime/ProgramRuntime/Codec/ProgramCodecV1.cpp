#include "ProgramCodecV1.h"

#include "mbedtls/sha256.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace savor::runtime::program {
namespace {

constexpr std::array<Byte, 4> kModuleMagic{'S', 'P', 'R', 'M'};
constexpr std::array<Byte, 4> kDependencyLockMagic{'S', 'P', 'R', 'D'};
constexpr std::array<Byte, 4> kInvocationMagic{'S', 'P', 'R', 'I'};
constexpr std::array<Byte, 4> kEmissionMagic{'S', 'P', 'R', 'E'};
constexpr std::array<Byte, 4> kResultMagic{'S', 'P', 'R', 'R'};

[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept
{
    const auto* cursor = reinterpret_cast<const unsigned char*>(text.data());
    const auto* const end = cursor + text.size();
    while (cursor != end)
    {
        const unsigned char lead = *cursor++;
        if (lead <= 0x7f)
            continue;

        std::uint32_t code_point = 0;
        std::size_t continuation_count = 0;
        if ((lead & 0xe0) == 0xc0)
        {
            code_point = lead & 0x1f;
            continuation_count = 1;
            if (code_point == 0)
                return false;
        }
        else if ((lead & 0xf0) == 0xe0)
        {
            code_point = lead & 0x0f;
            continuation_count = 2;
        }
        else if ((lead & 0xf8) == 0xf0)
        {
            code_point = lead & 0x07;
            continuation_count = 3;
        }
        else
        {
            return false;
        }

        if (static_cast<std::size_t>(end - cursor) < continuation_count)
            return false;
        for (std::size_t index = 0; index < continuation_count; ++index)
        {
            const unsigned char continuation = *cursor++;
            if ((continuation & 0xc0) != 0x80)
                return false;
            code_point = (code_point << 6) | (continuation & 0x3f);
        }

        if ((continuation_count == 1 && code_point < 0x80) ||
            (continuation_count == 2 && code_point < 0x800) ||
            (continuation_count == 3 && code_point < 0x10000) ||
            code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}

class Writer final
{
public:
    explicit Writer(const CodecLimits& limits)
        : limits_(limits)
    {
    }

    [[nodiscard]] const CodecStatus& status() const noexcept { return status_; }
    [[nodiscard]] const std::vector<Byte>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<Byte> TakeBytes() noexcept { return std::move(bytes_); }

    void Fail(CodecError error, std::string message)
    {
        if (status_)
            status_ = {error, std::move(message)};
    }

    void U8(std::uint8_t value) { Append(value); }
    void U16(std::uint16_t value)
    {
        Append(static_cast<Byte>(value));
        Append(static_cast<Byte>(value >> 8));
    }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
            Append(static_cast<Byte>(value >> shift));
    }
    void U64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift != 64; shift += 8)
            Append(static_cast<Byte>(value >> shift));
    }
    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void I64(std::int64_t value) { U64(std::bit_cast<std::uint64_t>(value)); }
    void F32(float value)
    {
        if (!std::isfinite(value))
        {
            Fail(CodecError::InvalidValue, "non-finite f32 is not canonical");
            return;
        }
        U32(std::bit_cast<std::uint32_t>(value));
    }
    void F64(double value)
    {
        if (!std::isfinite(value))
        {
            Fail(CodecError::InvalidValue, "non-finite f64 is not canonical");
            return;
        }
        U64(std::bit_cast<std::uint64_t>(value));
    }
    void Bool(bool value) { U8(value ? 1 : 0); }

    void Hash(const ContentHash256& hash)
    {
        Raw(hash.bytes);
    }

    void Raw(std::span<const Byte> bytes)
    {
        if (!status_)
            return;
        if (bytes.size() > limits_.maximum_payload_bytes ||
            bytes_.size() > limits_.maximum_payload_bytes - bytes.size())
        {
            Fail(CodecError::Oversized, "encoded payload exceeds configured limit");
            return;
        }
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void String(const std::string& text)
    {
        if (!IsValidUtf8(text))
        {
            Fail(CodecError::InvalidValue, "string is not canonical UTF-8");
            return;
        }
        if (text.size() > limits_.maximum_string_bytes ||
            text.size() > std::numeric_limits<std::uint32_t>::max())
        {
            Fail(CodecError::Oversized, "string exceeds configured limit");
            return;
        }
        U32(static_cast<std::uint32_t>(text.size()));
        Raw(std::span<const Byte>(
            reinterpret_cast<const Byte*>(text.data()),
            text.size()));
    }

    void Bytes(const std::vector<Byte>& bytes)
    {
        if (bytes.size() > limits_.maximum_string_bytes ||
            bytes.size() > std::numeric_limits<std::uint32_t>::max())
        {
            Fail(CodecError::Oversized, "byte string exceeds configured limit");
            return;
        }
        U32(static_cast<std::uint32_t>(bytes.size()));
        Raw(bytes);
    }

    template <typename Enum>
    void Enum(Enum value)
    {
        using Underlying = std::underlying_type_t<Enum>;
        if constexpr (sizeof(Underlying) == 1)
            U8(static_cast<std::uint8_t>(value));
        else if constexpr (sizeof(Underlying) == 2)
            U16(static_cast<std::uint16_t>(value));
        else
            U32(static_cast<std::uint32_t>(value));
    }

    template <typename T, typename Function>
    void Vector(const std::vector<T>& values, Function&& function)
    {
        if (values.size() > limits_.maximum_collection_elements ||
            values.size() > std::numeric_limits<std::uint32_t>::max())
        {
            Fail(CodecError::Oversized, "collection exceeds configured limit");
            return;
        }
        U32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values)
            function(*this, value);
    }

    template <typename T, typename Function>
    void Optional(const std::optional<T>& value, Function&& function)
    {
        Bool(value.has_value());
        if (value)
            function(*this, *value);
    }

private:
    void Append(Byte value)
    {
        if (!status_)
            return;
        if (bytes_.size() == limits_.maximum_payload_bytes)
        {
            Fail(CodecError::Oversized, "encoded payload exceeds configured limit");
            return;
        }
        bytes_.push_back(value);
    }

    CodecLimits limits_;
    CodecStatus status_;
    std::vector<Byte> bytes_;
};

class Reader final
{
public:
    Reader(std::span<const Byte> bytes, const CodecLimits& limits)
        : bytes_(bytes), limits_(limits)
    {
        if (bytes.size() > limits.maximum_payload_bytes)
            Fail(CodecError::Oversized, "payload exceeds configured limit");
    }

    [[nodiscard]] const CodecStatus& status() const noexcept { return status_; }
    [[nodiscard]] std::size_t consumed() const noexcept { return offset_; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }

    void Fail(CodecError error, std::string message)
    {
        if (status_)
            status_ = {error, std::move(message)};
    }

    bool U8(std::uint8_t& value)
    {
        if (!Require(1))
            return false;
        value = bytes_[offset_++];
        return true;
    }
    bool U16(std::uint16_t& value)
    {
        if (!Require(2))
            return false;
        value = static_cast<std::uint16_t>(bytes_[offset_]) |
            (static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8);
        offset_ += 2;
        return true;
    }
    bool U32(std::uint32_t& value)
    {
        if (!Require(4))
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        if (!Require(8))
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool I32(std::int32_t& value)
    {
        std::uint32_t raw = 0;
        if (!U32(raw))
            return false;
        value = std::bit_cast<std::int32_t>(raw);
        return true;
    }
    bool I64(std::int64_t& value)
    {
        std::uint64_t raw = 0;
        if (!U64(raw))
            return false;
        value = std::bit_cast<std::int64_t>(raw);
        return true;
    }
    bool F32(float& value)
    {
        std::uint32_t raw = 0;
        if (!U32(raw))
            return false;
        value = std::bit_cast<float>(raw);
        if (!std::isfinite(value))
        {
            Fail(CodecError::InvalidValue, "non-finite f32 is not canonical");
            return false;
        }
        return true;
    }
    bool F64(double& value)
    {
        std::uint64_t raw = 0;
        if (!U64(raw))
            return false;
        value = std::bit_cast<double>(raw);
        if (!std::isfinite(value))
        {
            Fail(CodecError::InvalidValue, "non-finite f64 is not canonical");
            return false;
        }
        return true;
    }
    bool Bool(bool& value)
    {
        std::uint8_t raw = 0;
        if (!U8(raw))
            return false;
        if (raw > 1)
        {
            Fail(CodecError::NonCanonical, "boolean must be encoded as zero or one");
            return false;
        }
        value = raw != 0;
        return true;
    }
    bool Hash(ContentHash256& hash)
    {
        if (!Require(hash.bytes.size()))
            return false;
        std::ranges::copy(
            bytes_.subspan(offset_, hash.bytes.size()),
            hash.bytes.begin());
        offset_ += hash.bytes.size();
        return true;
    }
    bool String(std::string& text)
    {
        std::uint32_t size = 0;
        if (!U32(size))
            return false;
        if (size > limits_.maximum_string_bytes)
        {
            Fail(CodecError::Oversized, "string exceeds configured limit");
            return false;
        }
        if (!Require(size))
            return false;
        text.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_),
            size);
        offset_ += size;
        if (!IsValidUtf8(text))
        {
            Fail(CodecError::InvalidValue, "string is not canonical UTF-8");
            return false;
        }
        return true;
    }
    bool Bytes(std::vector<Byte>& bytes)
    {
        std::uint32_t size = 0;
        if (!U32(size))
            return false;
        if (size > limits_.maximum_string_bytes)
        {
            Fail(CodecError::Oversized, "byte string exceeds configured limit");
            return false;
        }
        if (!Require(size))
            return false;
        bytes.assign(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return true;
    }

    template <typename Enum>
    bool EnumValue(Enum& value, Enum maximum)
    {
        using Underlying = std::underlying_type_t<Enum>;
        Underlying raw = 0;
        bool read = false;
        if constexpr (sizeof(Underlying) == 1)
        {
            std::uint8_t temporary = 0;
            read = U8(temporary);
            raw = static_cast<Underlying>(temporary);
        }
        else if constexpr (sizeof(Underlying) == 2)
        {
            std::uint16_t temporary = 0;
            read = U16(temporary);
            raw = static_cast<Underlying>(temporary);
        }
        else
        {
            std::uint32_t temporary = 0;
            read = U32(temporary);
            raw = static_cast<Underlying>(temporary);
        }
        if (!read)
            return false;
        if (raw > static_cast<Underlying>(maximum))
        {
            Fail(CodecError::InvalidValue, "enum value is outside its contract");
            return false;
        }
        value = static_cast<Enum>(raw);
        return true;
    }

    template <typename T, typename Function>
    bool Vector(std::vector<T>& values, Function&& function)
    {
        std::uint32_t count = 0;
        if (!U32(count))
            return false;
        if (count > limits_.maximum_collection_elements)
        {
            Fail(CodecError::Oversized, "collection exceeds configured limit");
            return false;
        }
        values.clear();
        values.reserve(count);
        for (std::uint32_t index = 0; index < count && status_; ++index)
        {
            T value{};
            if (!function(*this, value))
                return false;
            values.push_back(std::move(value));
        }
        return static_cast<bool>(status_);
    }

    template <typename T, typename Function>
    bool Optional(std::optional<T>& value, Function&& function)
    {
        bool present = false;
        if (!Bool(present))
            return false;
        if (!present)
        {
            value.reset();
            return true;
        }
        T decoded{};
        if (!function(*this, decoded))
            return false;
        value = std::move(decoded);
        return true;
    }

private:
    bool Require(std::size_t count)
    {
        if (!status_)
            return false;
        if (count > bytes_.size() - offset_)
        {
            Fail(CodecError::Truncated, "payload ended before the declared value");
            return false;
        }
        return true;
    }

    std::span<const Byte> bytes_;
    CodecLimits limits_;
    CodecStatus status_;
    std::size_t offset_ = 0;
};

template <typename Id>
void WriteId(Writer& writer, Id id)
{
    writer.U64(id.value());
}

template <typename Id>
bool ReadId(Reader& reader, Id& id)
{
    std::uint64_t value = 0;
    if (!reader.U64(value))
        return false;
    id = Id(value);
    return true;
}

void WriteSchemaIdentity(Writer& writer, const SchemaIdentity& identity)
{
    writer.String(identity.canonical_id);
    writer.U32(identity.version);
    writer.Hash(identity.schema_hash);
}

bool ReadSchemaIdentity(Reader& reader, SchemaIdentity& identity)
{
    return reader.String(identity.canonical_id) &&
        reader.U32(identity.version) &&
        reader.Hash(identity.schema_hash);
}

void WriteDependency(Writer& writer, const ExactDependencyIdentity& identity)
{
    writer.String(identity.canonical_id);
    writer.U32(identity.version);
    writer.Hash(identity.signature_hash);
}

bool ReadDependency(Reader& reader, ExactDependencyIdentity& identity)
{
    return reader.String(identity.canonical_id) &&
        reader.U32(identity.version) &&
        reader.Hash(identity.signature_hash);
}

void WriteModuleIdentity(
    Writer& writer,
    const ModuleIdentity& identity,
    CanonicalHashMode hash_mode)
{
    writer.String(identity.canonical_id);
    writer.U32(identity.revision);
    if (hash_mode == CanonicalHashMode::IncludeDeclaredHash)
        writer.Hash(identity.module_hash);
}

bool ReadModuleIdentity(Reader& reader, ModuleIdentity& identity)
{
    return reader.String(identity.canonical_id) &&
        reader.U32(identity.revision) &&
        reader.Hash(identity.module_hash);
}

void WriteModuleImport(Writer& writer, const ModuleImportIdentity& identity)
{
    WriteModuleIdentity(
        writer,
        identity.module,
        CanonicalHashMode::IncludeDeclaredHash);
    writer.U32(identity.ir_version);
}

bool ReadModuleImport(Reader& reader, ModuleImportIdentity& identity)
{
    return ReadModuleIdentity(reader, identity.module) &&
        reader.U32(identity.ir_version);
}

void WriteCapabilityPack(Writer& writer, const CapabilityPackIdentity& identity)
{
    writer.String(identity.canonical_id);
    writer.U32(identity.version);
    writer.Hash(identity.manifest_hash);
}

bool ReadCapabilityPack(Reader& reader, CapabilityPackIdentity& identity)
{
    return reader.String(identity.canonical_id) &&
        reader.U32(identity.version) &&
        reader.Hash(identity.manifest_hash);
}

void WriteTypeRef(Writer& writer, const TypeRef& type)
{
    writer.Bool(type.named.has_value());
    if (type.named)
        WriteSchemaIdentity(writer, *type.named);
    else
        writer.Enum(type.builtin);
}

bool ReadTypeRef(Reader& reader, TypeRef& type)
{
    bool named = false;
    if (!reader.Bool(named))
        return false;
    if (named)
    {
        SchemaIdentity identity;
        if (!ReadSchemaIdentity(reader, identity))
            return false;
        type = TypeRef::Named(std::move(identity));
        return true;
    }
    BuiltinType builtin = BuiltinType::Unit;
    if (!reader.EnumValue(builtin, BuiltinType::F64))
        return false;
    type = TypeRef::Builtin(builtin);
    return true;
}

void WriteEnumMember(Writer& writer, const EnumMemberDefinition& member)
{
    writer.String(member.name);
    writer.I64(member.value);
}

bool ReadEnumMember(Reader& reader, EnumMemberDefinition& member)
{
    return reader.String(member.name) && reader.I64(member.value);
}

void WriteRecordField(Writer& writer, const RecordFieldDefinition& field)
{
    writer.String(field.name);
    WriteTypeRef(writer, field.type);
}

bool ReadRecordField(Reader& reader, RecordFieldDefinition& field)
{
    if (!reader.String(field.name))
        return false;
    return ReadTypeRef(reader, field.type);
}

void WriteTypeSchema(Writer& writer, const TypeSchemaDefinition& schema)
{
    WriteSchemaIdentity(writer, schema.identity);
    writer.Enum(schema.kind);
    writer.U64(schema.maximum_size);
    writer.Optional(
        schema.element_type,
        [](Writer& output, const TypeRef& value) { WriteTypeRef(output, value); });
    writer.Vector(schema.enum_members, WriteEnumMember);
    writer.Vector(schema.record_fields, WriteRecordField);
    writer.Bool(schema.permits_incomplete);
}

bool ReadTypeSchema(Reader& reader, TypeSchemaDefinition& schema)
{
    if (!ReadSchemaIdentity(reader, schema.identity) ||
        !reader.EnumValue(
            schema.kind,
            TypeSchemaKind::EpochBoundOpaqueHandle) ||
        !reader.U64(schema.maximum_size) ||
        !reader.Optional(
            schema.element_type,
            [](Reader& input, TypeRef& value) {
                return ReadTypeRef(input, value);
            }) ||
        !reader.Vector(schema.enum_members, ReadEnumMember) ||
        !reader.Vector(schema.record_fields, ReadRecordField) ||
        !reader.Bool(schema.permits_incomplete))
    {
        return false;
    }
    return true;
}

void WriteLiteral(Writer& writer, const LiteralValue& literal)
{
    WriteTypeRef(writer, literal.type);
    writer.U8(static_cast<std::uint8_t>(literal.payload.index()));
    std::visit(
        [&writer](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, UnitValue>)
            {
            }
            else if constexpr (std::is_same_v<T, bool>)
                writer.Bool(value);
            else if constexpr (std::is_same_v<T, std::uint8_t>)
                writer.U8(value);
            else if constexpr (std::is_same_v<T, std::uint16_t>)
                writer.U16(value);
            else if constexpr (std::is_same_v<T, std::uint32_t>)
                writer.U32(value);
            else if constexpr (std::is_same_v<T, std::uint64_t>)
                writer.U64(value);
            else if constexpr (std::is_same_v<T, std::int32_t>)
                writer.I32(value);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                writer.I64(value);
            else if constexpr (std::is_same_v<T, float>)
                writer.F32(value);
            else if constexpr (std::is_same_v<T, double>)
                writer.F64(value);
            else if constexpr (std::is_same_v<T, std::string>)
                writer.String(value);
            else if constexpr (std::is_same_v<T, std::vector<Byte>>)
                writer.Bytes(value);
            else if constexpr (std::is_same_v<T, EnumValue>)
            {
                WriteSchemaIdentity(writer, value.schema);
                writer.I64(value.value);
            }
        },
        literal.payload);
}

bool ReadLiteral(Reader& reader, LiteralValue& literal)
{
    if (!ReadTypeRef(reader, literal.type))
        return false;
    std::uint8_t index = 0;
    if (!reader.U8(index) || index >= std::variant_size_v<LiteralPayload>)
    {
        if (reader.status())
            reader.Fail(CodecError::InvalidValue, "literal variant is invalid");
        return false;
    }
    switch (index)
    {
    case 0:
        literal.payload = UnitValue{};
        return true;
    case 1:
    {
        bool value = false;
        if (!reader.Bool(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 2:
    {
        std::uint8_t value = 0;
        if (!reader.U8(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 3:
    {
        std::uint16_t value = 0;
        if (!reader.U16(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 4:
    {
        std::uint32_t value = 0;
        if (!reader.U32(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 5:
    {
        std::uint64_t value = 0;
        if (!reader.U64(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 6:
    {
        std::int32_t value = 0;
        if (!reader.I32(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 7:
    {
        std::int64_t value = 0;
        if (!reader.I64(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 8:
    {
        float value = 0;
        if (!reader.F32(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 9:
    {
        double value = 0;
        if (!reader.F64(value))
            return false;
        literal.payload = value;
        return true;
    }
    case 10:
    {
        std::string value;
        if (!reader.String(value))
            return false;
        literal.payload = std::move(value);
        return true;
    }
    case 11:
    {
        std::vector<Byte> value;
        if (!reader.Bytes(value))
            return false;
        literal.payload = std::move(value);
        return true;
    }
    case 12:
    {
        EnumValue value;
        if (!ReadSchemaIdentity(reader, value.schema) ||
            !reader.I64(value.value))
        {
            return false;
        }
        literal.payload = std::move(value);
        return true;
    }
    default:
        reader.Fail(CodecError::InvalidValue, "literal variant is invalid");
        return false;
    }
}

void WriteArtifactReference(
    Writer& writer,
    const ArtifactReferenceValue& artifact)
{
    writer.String(artifact.artifact_id);
    WriteSchemaIdentity(writer, artifact.schema);
    writer.Hash(artifact.content_hash);
    writer.String(artifact.storage_reference);
    writer.Bool(artifact.complete);
}

bool ReadArtifactReference(
    Reader& reader,
    ArtifactReferenceValue& artifact)
{
    return reader.String(artifact.artifact_id) &&
        ReadSchemaIdentity(reader, artifact.schema) &&
        reader.Hash(artifact.content_hash) &&
        reader.String(artifact.storage_reference) &&
        reader.Bool(artifact.complete);
}

void WriteProgramValue(Writer& writer, const ProgramValue& value)
{
    WriteId(writer, value.id);
    WriteTypeRef(writer, value.type);
    writer.U8(static_cast<std::uint8_t>(value.payload.index()));
    std::visit(
        [&writer](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, UnitValue>)
            {
            }
            else if constexpr (std::is_same_v<T, bool>)
                writer.Bool(payload);
            else if constexpr (std::is_same_v<T, std::uint8_t>)
                writer.U8(payload);
            else if constexpr (std::is_same_v<T, std::uint16_t>)
                writer.U16(payload);
            else if constexpr (std::is_same_v<T, std::uint32_t>)
                writer.U32(payload);
            else if constexpr (std::is_same_v<T, std::uint64_t>)
                writer.U64(payload);
            else if constexpr (std::is_same_v<T, std::int32_t>)
                writer.I32(payload);
            else if constexpr (std::is_same_v<T, std::int64_t>)
                writer.I64(payload);
            else if constexpr (std::is_same_v<T, float>)
                writer.F32(payload);
            else if constexpr (std::is_same_v<T, double>)
                writer.F64(payload);
            else if constexpr (std::is_same_v<T, std::string>)
                writer.String(payload);
            else if constexpr (std::is_same_v<T, std::vector<Byte>>)
                writer.Bytes(payload);
            else if constexpr (std::is_same_v<T, EnumValue>)
            {
                WriteSchemaIdentity(writer, payload.schema);
                writer.I64(payload.value);
            }
            else if constexpr (std::is_same_v<T, OptionalValue>)
            {
                writer.Optional(
                    payload.value,
                    [](Writer& output, ProgramValueId id) {
                        WriteId(output, id);
                    });
            }
            else if constexpr (std::is_same_v<T, RecordValue>)
            {
                writer.Vector(
                    payload.fields,
                    [](Writer& output, ProgramValueId id) {
                        WriteId(output, id);
                    });
            }
            else if constexpr (std::is_same_v<T, ListValue>)
            {
                writer.Vector(
                    payload.elements,
                    [](Writer& output, ProgramValueId id) {
                        WriteId(output, id);
                    });
            }
            else if constexpr (std::is_same_v<T, ArtifactReferenceValue>)
                WriteArtifactReference(writer, payload);
            else if constexpr (std::is_same_v<T, ResourceHandleValue>)
            {
                WriteId(writer, payload.handle_id);
                WriteSchemaIdentity(writer, payload.resource_type);
                WriteId(writer, payload.workset_epoch);
            }
            else if constexpr (std::is_same_v<T, OpaqueHandleValue>)
            {
                WriteId(writer, payload.handle_id);
                WriteSchemaIdentity(writer, payload.handle_type);
                WriteId(writer, payload.workset_epoch);
            }
        },
        value.payload);
}

bool ReadProgramValue(Reader& reader, ProgramValue& value)
{
    if (!ReadId(reader, value.id) || !ReadTypeRef(reader, value.type))
        return false;
    std::uint8_t index = 0;
    if (!reader.U8(index) || index >= std::variant_size_v<ProgramValuePayload>)
    {
        if (reader.status())
            reader.Fail(CodecError::InvalidValue, "program value variant is invalid");
        return false;
    }

#define READ_SCALAR_VALUE(CASE, TYPE, METHOD) \
    case CASE:                                \
    {                                         \
        TYPE decoded{};                       \
        if (!reader.METHOD(decoded))           \
            return false;                     \
        value.payload = decoded;              \
        return true;                          \
    }

    switch (index)
    {
    case 0:
        value.payload = UnitValue{};
        return true;
    READ_SCALAR_VALUE(1, bool, Bool)
    READ_SCALAR_VALUE(2, std::uint8_t, U8)
    READ_SCALAR_VALUE(3, std::uint16_t, U16)
    READ_SCALAR_VALUE(4, std::uint32_t, U32)
    READ_SCALAR_VALUE(5, std::uint64_t, U64)
    READ_SCALAR_VALUE(6, std::int32_t, I32)
    READ_SCALAR_VALUE(7, std::int64_t, I64)
    READ_SCALAR_VALUE(8, float, F32)
    READ_SCALAR_VALUE(9, double, F64)
    case 10:
    {
        std::string decoded;
        if (!reader.String(decoded))
            return false;
        value.payload = std::move(decoded);
        return true;
    }
    case 11:
    {
        std::vector<Byte> decoded;
        if (!reader.Bytes(decoded))
            return false;
        value.payload = std::move(decoded);
        return true;
    }
    case 12:
    {
        EnumValue decoded;
        if (!ReadSchemaIdentity(reader, decoded.schema) ||
            !reader.I64(decoded.value))
        {
            return false;
        }
        value.payload = std::move(decoded);
        return true;
    }
    case 13:
    {
        OptionalValue decoded;
        if (!reader.Optional(
                decoded.value,
                [](Reader& input, ProgramValueId& id) {
                    return ReadId(input, id);
                }))
        {
            return false;
        }
        value.payload = std::move(decoded);
        return true;
    }
    case 14:
    {
        RecordValue decoded;
        if (!reader.Vector(
                decoded.fields,
                [](Reader& input, ProgramValueId& id) {
                    return ReadId(input, id);
                }))
        {
            return false;
        }
        value.payload = std::move(decoded);
        return true;
    }
    case 15:
    {
        ListValue decoded;
        if (!reader.Vector(
                decoded.elements,
                [](Reader& input, ProgramValueId& id) {
                    return ReadId(input, id);
                }))
        {
            return false;
        }
        value.payload = std::move(decoded);
        return true;
    }
    case 16:
    {
        ArtifactReferenceValue decoded;
        if (!ReadArtifactReference(reader, decoded))
            return false;
        value.payload = std::move(decoded);
        return true;
    }
    case 17:
    {
        ResourceHandleValue decoded;
        if (!ReadId(reader, decoded.handle_id) ||
            !ReadSchemaIdentity(reader, decoded.resource_type) ||
            !ReadId(reader, decoded.workset_epoch))
        {
            return false;
        }
        value.payload = std::move(decoded);
        return true;
    }
    case 18:
    {
        OpaqueHandleValue decoded;
        if (!ReadId(reader, decoded.handle_id) ||
            !ReadSchemaIdentity(reader, decoded.handle_type) ||
            !ReadId(reader, decoded.workset_epoch))
        {
            return false;
        }
        value.payload = std::move(decoded);
        return true;
    }
    default:
        reader.Fail(CodecError::InvalidValue, "program value variant is invalid");
        return false;
    }
#undef READ_SCALAR_VALUE
}

void WriteProgramValueGraph(Writer& writer, const ProgramValueGraph& graph)
{
    if (!graph.root)
    {
        writer.Fail(CodecError::InvalidValue, "value graph root must be nonzero");
        return;
    }
    std::set<std::uint64_t> ids;
    for (const auto& value : graph.values)
    {
        if (!value.id || !ids.insert(value.id.value()).second)
        {
            writer.Fail(
                CodecError::InvalidValue,
                "value graph IDs must be nonzero and unique");
            return;
        }
    }
    if (!ids.contains(graph.root.value()))
    {
        writer.Fail(CodecError::InvalidValue, "value graph root is unresolved");
        return;
    }
    auto require_id = [&](ProgramValueId id) {
        if (!id || !ids.contains(id.value()))
        {
            writer.Fail(
                CodecError::InvalidValue,
                "value graph contains an unresolved child ID");
            return false;
        }
        return true;
    };
    for (const auto& value : graph.values)
    {
        bool resolved = true;
        std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, OptionalValue>)
                {
                    if (payload.value)
                        resolved = require_id(*payload.value);
                }
                else if constexpr (std::is_same_v<T, RecordValue>)
                {
                    for (const auto id : payload.fields)
                        resolved = require_id(id) && resolved;
                }
                else if constexpr (std::is_same_v<T, ListValue>)
                {
                    for (const auto id : payload.elements)
                        resolved = require_id(id) && resolved;
                }
            },
            value.payload);
        if (!resolved)
            return;
    }
    std::map<std::uint64_t, const ProgramValue*> by_id;
    for (const auto& value : graph.values)
        by_id.emplace(value.id.value(), &value);
    std::map<std::uint64_t, std::uint8_t> state;
    std::vector<std::pair<ProgramValueId, bool>> stack{
        {graph.root, false},
    };
    while (!stack.empty())
    {
        const auto [id, exiting] = stack.back();
        stack.pop_back();
        auto& visit_state = state[id.value()];
        if (exiting)
        {
            visit_state = 2;
            continue;
        }
        if (visit_state == 2)
            continue;
        if (visit_state == 1)
        {
            writer.Fail(
                CodecError::InvalidValue,
                "value graph contains a cycle");
            return;
        }
        visit_state = 1;
        stack.emplace_back(id, true);
        const auto* value = by_id.at(id.value());
        std::vector<ProgramValueId> children;
        std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, OptionalValue>)
                {
                    if (payload.value)
                        children.push_back(*payload.value);
                }
                else if constexpr (std::is_same_v<T, RecordValue>)
                {
                    children = payload.fields;
                }
                else if constexpr (std::is_same_v<T, ListValue>)
                {
                    children = payload.elements;
                }
            },
            value->payload);
        for (auto child = children.rbegin(); child != children.rend(); ++child)
            stack.emplace_back(*child, false);
    }
    if (std::ranges::count_if(
            state,
            [](const auto& entry) { return entry.second == 2; }) !=
        static_cast<std::ptrdiff_t>(graph.values.size()))
    {
        writer.Fail(
            CodecError::InvalidValue,
            "value graph contains unreachable values");
        return;
    }
    WriteId(writer, graph.root);
    auto values = graph.values;
    std::ranges::sort(
        values,
        {},
        [](const ProgramValue& value) { return value.id.value(); });
    writer.Vector(values, WriteProgramValue);
}

bool ReadProgramValueGraph(Reader& reader, ProgramValueGraph& graph)
{
    return ReadId(reader, graph.root) &&
        reader.Vector(graph.values, ReadProgramValue);
}

void WriteValueDefinition(Writer& writer, const ValueDefinition& definition)
{
    WriteId(writer, definition.id);
    WriteTypeRef(writer, definition.type);
}

bool ReadValueDefinition(Reader& reader, ValueDefinition& definition)
{
    return ReadId(reader, definition.id) &&
        ReadTypeRef(reader, definition.type);
}

void WriteInstructionTarget(Writer& writer, const InstructionTarget& target)
{
    writer.Enum(target.kind);
    WriteId(writer, target.local_function);
    writer.Optional(target.dependency, WriteDependency);
    writer.String(target.member_name);
}

bool ReadInstructionTarget(Reader& reader, InstructionTarget& target)
{
    return reader.EnumValue(target.kind, InstructionTargetKind::DeferredAction) &&
        ReadId(reader, target.local_function) &&
        reader.Optional(target.dependency, ReadDependency) &&
        reader.String(target.member_name);
}

void WriteInstruction(Writer& writer, const Instruction& instruction)
{
    WriteId(writer, instruction.id);
    writer.Enum(instruction.opcode);
    WriteId(writer, instruction.source_location);
    writer.Optional(instruction.result, WriteValueDefinition);
    writer.Vector(
        instruction.operands,
        [](Writer& output, ProgramValueId value) { WriteId(output, value); });
    writer.Optional(instruction.literal, WriteLiteral);
    WriteInstructionTarget(writer, instruction.target);
    writer.String(instruction.selector);
    writer.U64(instruction.ordinal);
    WriteId(writer, instruction.scope);
}

bool ReadInstruction(Reader& reader, Instruction& instruction)
{
    return ReadId(reader, instruction.id) &&
        reader.EnumValue(instruction.opcode, InstructionOpcode::PromoteResource) &&
        ReadId(reader, instruction.source_location) &&
        reader.Optional(instruction.result, ReadValueDefinition) &&
        reader.Vector(
            instruction.operands,
            [](Reader& input, ProgramValueId& value) {
                return ReadId(input, value);
            }) &&
        reader.Optional(instruction.literal, ReadLiteral) &&
        ReadInstructionTarget(reader, instruction.target) &&
        reader.String(instruction.selector) &&
        reader.U64(instruction.ordinal) &&
        ReadId(reader, instruction.scope);
}

void WriteBlockEdge(Writer& writer, const BlockEdge& edge)
{
    WriteId(writer, edge.target);
    writer.Vector(
        edge.arguments,
        [](Writer& output, ProgramValueId value) { WriteId(output, value); });
}

bool ReadBlockEdge(Reader& reader, BlockEdge& edge)
{
    return ReadId(reader, edge.target) &&
        reader.Vector(
            edge.arguments,
            [](Reader& input, ProgramValueId& value) {
                return ReadId(input, value);
            });
}

void WriteEnumSwitchCase(Writer& writer, const EnumSwitchCase& switch_case)
{
    writer.I64(switch_case.enum_value);
    WriteBlockEdge(writer, switch_case.edge);
}

bool ReadEnumSwitchCase(Reader& reader, EnumSwitchCase& switch_case)
{
    return reader.I64(switch_case.enum_value) &&
        ReadBlockEdge(reader, switch_case.edge);
}

void WriteStructuredFailure(Writer& writer, const StructuredFailure& failure)
{
    writer.String(failure.code);
    writer.String(failure.message);
    writer.Optional(
        failure.details,
        [](Writer& output, ProgramValueId value) {
            WriteId(output, value);
        });
}

bool ReadStructuredFailure(Reader& reader, StructuredFailure& failure)
{
    return reader.String(failure.code) &&
        reader.String(failure.message) &&
        reader.Optional(
            failure.details,
            [](Reader& input, ProgramValueId& value) {
                return ReadId(input, value);
            });
}

void WriteTerminator(Writer& writer, const Terminator& terminator)
{
    writer.Enum(terminator.kind);
    WriteId(writer, terminator.source_location);
    writer.Optional(
        terminator.condition_or_selector,
        [](Writer& output, ProgramValueId value) {
            WriteId(output, value);
        });
    writer.Vector(terminator.edges, WriteBlockEdge);
    writer.Vector(terminator.enum_cases, WriteEnumSwitchCase);
    writer.Optional(terminator.default_edge, WriteBlockEdge);
    writer.Optional(
        terminator.return_value,
        [](Writer& output, ProgramValueId value) {
            WriteId(output, value);
        });
    writer.Optional(
        terminator.domain_outcome,
        [](Writer& output, ProgramValueId value) {
            WriteId(output, value);
        });
    writer.Optional(terminator.failure, WriteStructuredFailure);
}

bool ReadTerminator(Reader& reader, Terminator& terminator)
{
    return reader.EnumValue(terminator.kind, TerminatorKind::StructuredFail) &&
        ReadId(reader, terminator.source_location) &&
        reader.Optional(
            terminator.condition_or_selector,
            [](Reader& input, ProgramValueId& value) {
                return ReadId(input, value);
            }) &&
        reader.Vector(terminator.edges, ReadBlockEdge) &&
        reader.Vector(terminator.enum_cases, ReadEnumSwitchCase) &&
        reader.Optional(terminator.default_edge, ReadBlockEdge) &&
        reader.Optional(
            terminator.return_value,
            [](Reader& input, ProgramValueId& value) {
                return ReadId(input, value);
            }) &&
        reader.Optional(
            terminator.domain_outcome,
            [](Reader& input, ProgramValueId& value) {
                return ReadId(input, value);
            }) &&
        reader.Optional(terminator.failure, ReadStructuredFailure);
}

void WriteBlock(Writer& writer, const BasicBlock& block)
{
    WriteId(writer, block.id);
    writer.Vector(block.arguments, WriteValueDefinition);
    writer.Vector(block.instructions, WriteInstruction);
    WriteTerminator(writer, block.terminator);
}

bool ReadBlock(Reader& reader, BasicBlock& block)
{
    return ReadId(reader, block.id) &&
        reader.Vector(block.arguments, ReadValueDefinition) &&
        reader.Vector(block.instructions, ReadInstruction) &&
        ReadTerminator(reader, block.terminator);
}

void WriteFunction(Writer& writer, const ProgramFunction& function)
{
    WriteId(writer, function.id);
    writer.String(function.name);
    writer.Vector(function.arguments, WriteValueDefinition);
    WriteTypeRef(writer, function.output_type);
    writer.Optional(
        function.domain_outcome_type,
        [](Writer& output, const TypeRef& type) {
            WriteTypeRef(output, type);
        });
    WriteId(writer, function.entry_block);
    writer.Vector(function.blocks, WriteBlock);
    writer.Bool(function.exported);
}

bool ReadFunction(Reader& reader, ProgramFunction& function)
{
    return ReadId(reader, function.id) &&
        reader.String(function.name) &&
        reader.Vector(function.arguments, ReadValueDefinition) &&
        ReadTypeRef(reader, function.output_type) &&
        reader.Optional(
            function.domain_outcome_type,
            [](Reader& input, TypeRef& type) {
                return ReadTypeRef(input, type);
            }) &&
        ReadId(reader, function.entry_block) &&
        reader.Vector(function.blocks, ReadBlock) &&
        reader.Bool(function.exported);
}

void WriteBudgets(Writer& writer, const ProgramBudgets& budgets)
{
    writer.U64(budgets.maximum_instructions);
    writer.U64(budgets.maximum_calls);
    writer.U64(budgets.maximum_call_depth);
    writer.U64(budgets.maximum_action_requests);
    writer.U64(budgets.maximum_emissions);
    writer.U64(budgets.maximum_artifacts);
    writer.U64(budgets.maximum_values);
    writer.U64(budgets.maximum_value_bytes);
    writer.U64(budgets.maximum_trace_events);
}

bool ReadBudgets(Reader& reader, ProgramBudgets& budgets)
{
    return reader.U64(budgets.maximum_instructions) &&
        reader.U64(budgets.maximum_calls) &&
        reader.U64(budgets.maximum_call_depth) &&
        reader.U64(budgets.maximum_action_requests) &&
        reader.U64(budgets.maximum_emissions) &&
        reader.U64(budgets.maximum_artifacts) &&
        reader.U64(budgets.maximum_values) &&
        reader.U64(budgets.maximum_value_bytes) &&
        reader.U64(budgets.maximum_trace_events);
}

void WritePolicySet(Writer& writer, const ProgramPolicySet& policies)
{
    if (std::set<InvocationStatePolicy>(
            policies.state_policies.begin(),
            policies.state_policies.end()).size() !=
            policies.state_policies.size() ||
        std::set<ExecutionIntent>(
            policies.execution_intents.begin(),
            policies.execution_intents.end()).size() !=
            policies.execution_intents.size())
    {
        writer.Fail(CodecError::InvalidValue, "policy set contains duplicates");
        return;
    }
    writer.Vector(
        policies.state_policies,
        [](Writer& output, InvocationStatePolicy policy) {
            output.Enum(policy);
        });
    writer.Vector(
        policies.execution_intents,
        [](Writer& output, ExecutionIntent intent) {
            output.Enum(intent);
        });
    writer.Bool(policies.permits_movie_playback);
    writer.Bool(policies.permits_movie_recording);
    writer.Bool(policies.permits_capture);
    writer.Bool(policies.permits_replay);
    writer.Bool(policies.permits_resource_promotion);
}

bool ReadPolicySet(Reader& reader, ProgramPolicySet& policies)
{
    return reader.Vector(
               policies.state_policies,
               [](Reader& input, InvocationStatePolicy& policy) {
                   return input.EnumValue(
                       policy,
                       InvocationStatePolicy::EstablishBaseline);
               }) &&
        reader.Vector(
            policies.execution_intents,
            [](Reader& input, ExecutionIntent& intent) {
                return input.EnumValue(intent, ExecutionIntent::Replay);
            }) &&
        reader.Bool(policies.permits_movie_playback) &&
        reader.Bool(policies.permits_movie_recording) &&
        reader.Bool(policies.permits_capture) &&
        reader.Bool(policies.permits_replay) &&
        reader.Bool(policies.permits_resource_promotion);
}

void WriteEntrypoint(Writer& writer, const ProgramEntrypoint& entrypoint)
{
    writer.String(entrypoint.name);
    WriteId(writer, entrypoint.function);
    WriteTypeRef(writer, entrypoint.input_type);
    WriteTypeRef(writer, entrypoint.output_type);
    WriteTypeRef(writer, entrypoint.domain_outcome_type);
    writer.Vector(entrypoint.emission_schemas, WriteSchemaIdentity);
    writer.Vector(entrypoint.artifact_schemas, WriteSchemaIdentity);
    writer.Vector(
        entrypoint.required_capability_packs,
        WriteCapabilityPack);
    WritePolicySet(writer, entrypoint.accepted_policies);
    writer.Optional(entrypoint.narrowed_budgets, WriteBudgets);
}

bool ReadEntrypoint(Reader& reader, ProgramEntrypoint& entrypoint)
{
    return reader.String(entrypoint.name) &&
        ReadId(reader, entrypoint.function) &&
        ReadTypeRef(reader, entrypoint.input_type) &&
        ReadTypeRef(reader, entrypoint.output_type) &&
        ReadTypeRef(reader, entrypoint.domain_outcome_type) &&
        reader.Vector(entrypoint.emission_schemas, ReadSchemaIdentity) &&
        reader.Vector(entrypoint.artifact_schemas, ReadSchemaIdentity) &&
        reader.Vector(
            entrypoint.required_capability_packs,
            ReadCapabilityPack) &&
        ReadPolicySet(reader, entrypoint.accepted_policies) &&
        reader.Optional(entrypoint.narrowed_budgets, ReadBudgets);
}

void WriteSourceMapEntry(Writer& writer, const SourceMapEntry& entry)
{
    WriteId(writer, entry.id);
    writer.Optional(
        entry.function,
        [](Writer& output, ProgramFunctionId id) { WriteId(output, id); });
    writer.Optional(
        entry.block,
        [](Writer& output, ProgramBlockId id) { WriteId(output, id); });
    writer.Optional(
        entry.instruction,
        [](Writer& output, ProgramInstructionId id) {
            WriteId(output, id);
        });
    writer.String(entry.source_name);
    writer.String(entry.semantic_path);
    writer.U32(entry.line);
    writer.U32(entry.column);
}

bool ReadSourceMapEntry(Reader& reader, SourceMapEntry& entry)
{
    return ReadId(reader, entry.id) &&
        reader.Optional(
            entry.function,
            [](Reader& input, ProgramFunctionId& id) {
                return ReadId(input, id);
            }) &&
        reader.Optional(
            entry.block,
            [](Reader& input, ProgramBlockId& id) {
                return ReadId(input, id);
            }) &&
        reader.Optional(
            entry.instruction,
            [](Reader& input, ProgramInstructionId& id) {
                return ReadId(input, id);
            }) &&
        reader.String(entry.source_name) &&
        reader.String(entry.semantic_path) &&
        reader.U32(entry.line) &&
        reader.U32(entry.column);
}

void WriteSourceMap(Writer& writer, const ProgramSourceMap& source_map)
{
    writer.U32(source_map.version);
    writer.Vector(source_map.entries, WriteSourceMapEntry);
}

bool ReadSourceMap(Reader& reader, ProgramSourceMap& source_map)
{
    return reader.U32(source_map.version) &&
        reader.Vector(source_map.entries, ReadSourceMapEntry);
}

template <typename T, typename Projection>
void SortUniqueSemanticVector(std::vector<T>& values, Projection projection)
{
    std::ranges::sort(values, {}, projection);
}

ProgramPolicySet CanonicalPolicySet(ProgramPolicySet policies)
{
    std::ranges::sort(policies.state_policies);
    std::ranges::sort(policies.execution_intents);
    return policies;
}

ProgramModule CanonicalModule(ProgramModule module)
{
    std::ranges::sort(
        module.entrypoints,
        {},
        &ProgramEntrypoint::name);
    for (auto& entrypoint : module.entrypoints)
    {
        std::ranges::sort(
            entrypoint.emission_schemas,
            {},
            [](const SchemaIdentity& identity) {
                return std::tie(
                    identity.canonical_id,
                    identity.version,
                    identity.schema_hash);
            });
        std::ranges::sort(
            entrypoint.artifact_schemas,
            {},
            [](const SchemaIdentity& identity) {
                return std::tie(
                    identity.canonical_id,
                    identity.version,
                    identity.schema_hash);
            });
        std::ranges::sort(
            entrypoint.required_capability_packs,
            {},
            [](const CapabilityPackIdentity& identity) {
                return std::tie(
                    identity.canonical_id,
                    identity.version,
                    identity.manifest_hash);
            });
        entrypoint.accepted_policies =
            CanonicalPolicySet(std::move(entrypoint.accepted_policies));
    }
    std::ranges::sort(
        module.functions,
        {},
        [](const ProgramFunction& function) { return function.id.value(); });
    for (auto& function : module.functions)
    {
        std::ranges::sort(
            function.blocks,
            {},
            [](const BasicBlock& block) { return block.id.value(); });
        for (auto& block : function.blocks)
        {
            if (block.terminator.kind == TerminatorKind::EnumSwitch)
            {
                std::ranges::sort(
                    block.terminator.enum_cases,
                    {},
                    &EnumSwitchCase::enum_value);
            }
        }
    }
    std::ranges::sort(
        module.local_types,
        {},
        [](const TypeSchemaDefinition& type) {
            return std::tie(type.identity.canonical_id, type.identity.version);
        });
    std::ranges::sort(
        module.module_imports,
        {},
        [](const ModuleImportIdentity& dependency) {
            return std::tie(
                dependency.module.canonical_id,
                dependency.module.revision);
        });
    auto exact_key = [](const ExactDependencyIdentity& dependency) {
        return std::tie(dependency.canonical_id, dependency.version);
    };
    std::ranges::sort(module.action_imports, {}, exact_key);
    std::ranges::sort(module.reducer_imports, {}, exact_key);
    std::ranges::sort(
        module.type_imports,
        {},
        [](const SchemaIdentity& dependency) {
            return std::tie(dependency.canonical_id, dependency.version);
        });
    std::ranges::sort(
        module.required_capability_packs,
        {},
        [](const CapabilityPackIdentity& dependency) {
            return std::tie(dependency.canonical_id, dependency.version);
        });
    module.accepted_policies =
        CanonicalPolicySet(std::move(module.accepted_policies));
    std::ranges::sort(
        module.source_map.entries,
        {},
        [](const SourceMapEntry& entry) { return entry.id.value(); });
    return module;
}

ProgramDependencyLock CanonicalDependencyLock(ProgramDependencyLock lock)
{
    std::ranges::sort(
        lock.module_imports,
        {},
        [](const ModuleImportIdentity& dependency) {
            return std::tie(
                dependency.module.canonical_id,
                dependency.module.revision);
        });
    auto exact_key = [](const ExactDependencyIdentity& dependency) {
        return std::tie(dependency.canonical_id, dependency.version);
    };
    std::ranges::sort(lock.action_imports, {}, exact_key);
    std::ranges::sort(lock.reducer_imports, {}, exact_key);
    std::ranges::sort(
        lock.type_imports,
        {},
        [](const SchemaIdentity& dependency) {
            return std::tie(dependency.canonical_id, dependency.version);
        });
    std::ranges::sort(
        lock.capability_packs,
        {},
        [](const CapabilityPackIdentity& dependency) {
            return std::tie(dependency.canonical_id, dependency.version);
        });
    return lock;
}

void WriteModuleBody(
    Writer& writer,
    const ProgramModule& source,
    CanonicalHashMode hash_mode)
{
    const ProgramModule module = CanonicalModule(source);
    WriteModuleIdentity(writer, module.identity, hash_mode);
    writer.U32(module.ir_version);
    writer.Vector(module.entrypoints, WriteEntrypoint);
    writer.Vector(module.functions, WriteFunction);
    writer.Vector(module.local_types, WriteTypeSchema);
    writer.Vector(module.module_imports, WriteModuleImport);
    writer.Vector(module.action_imports, WriteDependency);
    writer.Vector(module.reducer_imports, WriteDependency);
    writer.Vector(module.type_imports, WriteSchemaIdentity);
    writer.Vector(module.required_capability_packs, WriteCapabilityPack);
    WritePolicySet(writer, module.accepted_policies);
    WriteBudgets(writer, module.budgets);
    WriteSourceMap(writer, module.source_map);
}

bool ReadModuleBody(Reader& reader, ProgramModule& module)
{
    return ReadModuleIdentity(reader, module.identity) &&
        reader.U32(module.ir_version) &&
        reader.Vector(module.entrypoints, ReadEntrypoint) &&
        reader.Vector(module.functions, ReadFunction) &&
        reader.Vector(module.local_types, ReadTypeSchema) &&
        reader.Vector(module.module_imports, ReadModuleImport) &&
        reader.Vector(module.action_imports, ReadDependency) &&
        reader.Vector(module.reducer_imports, ReadDependency) &&
        reader.Vector(module.type_imports, ReadSchemaIdentity) &&
        reader.Vector(
            module.required_capability_packs,
            ReadCapabilityPack) &&
        ReadPolicySet(reader, module.accepted_policies) &&
        ReadBudgets(reader, module.budgets) &&
        ReadSourceMap(reader, module.source_map);
}

void WriteDependencyLock(Writer& writer, const ProgramDependencyLock& source)
{
    const auto lock = CanonicalDependencyLock(source);
    auto duplicate_exact = [](const std::vector<ExactDependencyIdentity>& values) {
        return std::ranges::adjacent_find(
                   values,
                   [](const auto& left, const auto& right) {
                       return left.canonical_id == right.canonical_id &&
                           left.version == right.version;
                   }) != values.end();
    };
    auto duplicate_module = std::ranges::adjacent_find(
        lock.module_imports,
        [](const auto& left, const auto& right) {
            return left.module.canonical_id == right.module.canonical_id &&
                left.module.revision == right.module.revision;
        });
    auto duplicate_type = std::ranges::adjacent_find(
        lock.type_imports,
        [](const auto& left, const auto& right) {
            return left.canonical_id == right.canonical_id &&
                left.version == right.version;
        });
    auto duplicate_pack = std::ranges::adjacent_find(
        lock.capability_packs,
        [](const auto& left, const auto& right) {
            return left.canonical_id == right.canonical_id &&
                left.version == right.version;
        });
    if (duplicate_module != lock.module_imports.end() ||
        duplicate_exact(lock.action_imports) ||
        duplicate_exact(lock.reducer_imports) ||
        duplicate_type != lock.type_imports.end() ||
        duplicate_pack != lock.capability_packs.end())
    {
        writer.Fail(
            CodecError::InvalidValue,
            "dependency lock contains duplicate identities");
        return;
    }
    writer.U32(lock.ir_version);
    writer.Vector(lock.module_imports, WriteModuleImport);
    writer.Vector(lock.action_imports, WriteDependency);
    writer.Vector(lock.reducer_imports, WriteDependency);
    writer.Vector(lock.type_imports, WriteSchemaIdentity);
    writer.Vector(lock.capability_packs, WriteCapabilityPack);
}

bool ReadDependencyLock(Reader& reader, ProgramDependencyLock& lock)
{
    return reader.U32(lock.ir_version) &&
        reader.Vector(lock.module_imports, ReadModuleImport) &&
        reader.Vector(lock.action_imports, ReadDependency) &&
        reader.Vector(lock.reducer_imports, ReadDependency) &&
        reader.Vector(lock.type_imports, ReadSchemaIdentity) &&
        reader.Vector(lock.capability_packs, ReadCapabilityPack);
}

void WriteRuntimeProfile(Writer& writer, const RuntimeProfile& source)
{
    RuntimeProfile profile = source;
    std::ranges::sort(
        profile.capability_packs,
        {},
        [](const CapabilityPackIdentity& identity) {
            return std::tie(identity.canonical_id, identity.version);
        });
    if (std::ranges::adjacent_find(
            profile.capability_packs,
            [](const auto& left, const auto& right) {
                return left.canonical_id == right.canonical_id &&
                    left.version == right.version;
            }) != profile.capability_packs.end())
    {
        writer.Fail(
            CodecError::InvalidValue,
            "runtime profile contains duplicate capability packs");
        return;
    }
    writer.String(profile.profile_id);
    writer.String(profile.game_id);
    writer.String(profile.disc_identity);
    writer.String(profile.executable_identity);
    writer.String(profile.backend);
    writer.Vector(profile.capability_packs, WriteCapabilityPack);
}

bool ReadRuntimeProfile(Reader& reader, RuntimeProfile& profile)
{
    return reader.String(profile.profile_id) &&
        reader.String(profile.game_id) &&
        reader.String(profile.disc_identity) &&
        reader.String(profile.executable_identity) &&
        reader.String(profile.backend) &&
        reader.Vector(profile.capability_packs, ReadCapabilityPack);
}

void WriteStateRequest(Writer& writer, const InvocationStateRequest& state)
{
    writer.Enum(state.policy);
    writer.String(state.session_lineage);
    WriteId(writer, state.expected_session);
    WriteId(writer, state.expected_epoch);
}

bool ReadStateRequest(Reader& reader, InvocationStateRequest& state)
{
    return reader.EnumValue(
               state.policy,
               InvocationStatePolicy::EstablishBaseline) &&
        reader.String(state.session_lineage) &&
        ReadId(reader, state.expected_session) &&
        ReadId(reader, state.expected_epoch);
}

void WriteExecutionPolicy(
    Writer& writer,
    const InvocationExecutionPolicy& execution)
{
    writer.Enum(execution.intent);
    writer.Bool(execution.allow_movie_playback);
    writer.Bool(execution.allow_movie_recording);
    writer.Bool(execution.allow_input);
    writer.Bool(execution.allow_capture);
    writer.Bool(execution.record_trace);
}

bool ReadExecutionPolicy(
    Reader& reader,
    InvocationExecutionPolicy& execution)
{
    return reader.EnumValue(execution.intent, ExecutionIntent::Replay) &&
        reader.Bool(execution.allow_movie_playback) &&
        reader.Bool(execution.allow_movie_recording) &&
        reader.Bool(execution.allow_input) &&
        reader.Bool(execution.allow_capture) &&
        reader.Bool(execution.record_trace);
}

void WriteProvenanceEntry(Writer& writer, const ProvenanceEntry& entry)
{
    writer.String(entry.key);
    writer.String(entry.value);
}

bool ReadProvenanceEntry(Reader& reader, ProvenanceEntry& entry)
{
    return reader.String(entry.key) && reader.String(entry.value);
}

void WriteProvenance(Writer& writer, const ProgramProvenance& source)
{
    ProgramProvenance provenance = source;
    std::ranges::sort(
        provenance.source_artifacts,
        {},
        &ArtifactReferenceValue::artifact_id);
    if (std::ranges::adjacent_find(
            provenance.source_artifacts,
            {},
            &ArtifactReferenceValue::artifact_id) !=
        provenance.source_artifacts.end())
    {
        writer.Fail(
            CodecError::InvalidValue,
            "provenance contains duplicate source artifact IDs");
        return;
    }
    std::ranges::sort(
        provenance.attributes,
        {},
        [](const ProvenanceEntry& entry) {
            return std::tie(entry.key, entry.value);
        });
    writer.String(provenance.requesting_component);
    writer.Vector(provenance.source_artifacts, WriteArtifactReference);
    writer.Vector(provenance.attributes, WriteProvenanceEntry);
}

bool ReadProvenance(Reader& reader, ProgramProvenance& provenance)
{
    return reader.String(provenance.requesting_component) &&
        reader.Vector(
            provenance.source_artifacts,
            ReadArtifactReference) &&
        reader.Vector(provenance.attributes, ReadProvenanceEntry);
}

void WriteInvocationBody(Writer& writer, const ProgramInvocation& invocation)
{
    WriteId(writer, invocation.invocation_id);
    WriteId(writer, invocation.attempt_id);
    WriteModuleIdentity(
        writer,
        invocation.module,
        CanonicalHashMode::IncludeDeclaredHash);
    writer.String(invocation.entrypoint);
    WriteDependencyLock(writer, invocation.dependencies);
    WriteRuntimeProfile(writer, invocation.runtime_profile);
    WriteStateRequest(writer, invocation.state);
    WriteExecutionPolicy(writer, invocation.execution);
    WriteProgramValueGraph(writer, invocation.input);
    WriteBudgets(writer, invocation.limits);
    WriteProvenance(writer, invocation.provenance);
}

bool ReadInvocationBody(Reader& reader, ProgramInvocation& invocation)
{
    return ReadId(reader, invocation.invocation_id) &&
        ReadId(reader, invocation.attempt_id) &&
        ReadModuleIdentity(reader, invocation.module) &&
        reader.String(invocation.entrypoint) &&
        ReadDependencyLock(reader, invocation.dependencies) &&
        ReadRuntimeProfile(reader, invocation.runtime_profile) &&
        ReadStateRequest(reader, invocation.state) &&
        ReadExecutionPolicy(reader, invocation.execution) &&
        ReadProgramValueGraph(reader, invocation.input) &&
        ReadBudgets(reader, invocation.limits) &&
        ReadProvenance(reader, invocation.provenance);
}

void WriteDiagnostic(Writer& writer, const ProgramDiagnostic& diagnostic)
{
    writer.Enum(diagnostic.severity);
    writer.String(diagnostic.code);
    writer.String(diagnostic.message);
    writer.Optional(
        diagnostic.source_location,
        [](Writer& output, ProgramSourceLocationId id) {
            WriteId(output, id);
        });
    writer.Vector(
        diagnostic.causal_chain,
        [](Writer& output, const std::string& value) {
            output.String(value);
        });
}

bool ReadDiagnostic(Reader& reader, ProgramDiagnostic& diagnostic)
{
    return reader.EnumValue(
               diagnostic.severity,
               DiagnosticSeverity::Error) &&
        reader.String(diagnostic.code) &&
        reader.String(diagnostic.message) &&
        reader.Optional(
            diagnostic.source_location,
            [](Reader& input, ProgramSourceLocationId& id) {
                return ReadId(input, id);
            }) &&
        reader.Vector(
            diagnostic.causal_chain,
            [](Reader& input, std::string& value) {
                return input.String(value);
            });
}

void WriteEmission(Writer& writer, const ProgramEmission& emission)
{
    WriteId(writer, emission.sequence);
    WriteSchemaIdentity(writer, emission.schema);
    WriteProgramValueGraph(writer, emission.value);
    writer.Bool(emission.complete);
}

bool ReadEmission(Reader& reader, ProgramEmission& emission)
{
    return ReadId(reader, emission.sequence) &&
        ReadSchemaIdentity(reader, emission.schema) &&
        ReadProgramValueGraph(reader, emission.value) &&
        reader.Bool(emission.complete);
}

void WriteArtifact(Writer& writer, const ProgramArtifact& artifact)
{
    WriteId(writer, artifact.sequence);
    WriteArtifactReference(writer, artifact.artifact);
}

bool ReadArtifact(Reader& reader, ProgramArtifact& artifact)
{
    return ReadId(reader, artifact.sequence) &&
        ReadArtifactReference(reader, artifact.artifact);
}

void WriteTraceEvent(Writer& writer, const ProgramTraceEvent& event)
{
    WriteId(writer, event.sequence);
    writer.String(event.kind);
    writer.Optional(
        event.source_location,
        [](Writer& output, ProgramSourceLocationId id) {
            WriteId(output, id);
        });
    WriteId(writer, event.epoch);
    auto attributes = event.attributes;
    std::ranges::sort(
        attributes,
        {},
        [](const ProvenanceEntry& entry) {
            return std::tie(entry.key, entry.value);
        });
    writer.Vector(attributes, WriteProvenanceEntry);
}

bool ReadTraceEvent(Reader& reader, ProgramTraceEvent& event)
{
    return ReadId(reader, event.sequence) &&
        reader.String(event.kind) &&
        reader.Optional(
            event.source_location,
            [](Reader& input, ProgramSourceLocationId& id) {
                return ReadId(input, id);
            }) &&
        ReadId(reader, event.epoch) &&
        reader.Vector(event.attributes, ReadProvenanceEntry);
}

void WriteCleanupReceipt(Writer& writer, const CleanupReceipt& receipt)
{
    WriteId(writer, receipt.resource);
    writer.Enum(receipt.status);
    writer.String(receipt.diagnostic);
}

bool ReadCleanupReceipt(Reader& reader, CleanupReceipt& receipt)
{
    return ReadId(reader, receipt.resource) &&
        reader.EnumValue(receipt.status, ProgramCleanupStatus::Tainted) &&
        reader.String(receipt.diagnostic);
}

void WriteResultBody(Writer& writer, const ProgramResult& result)
{
    WriteId(writer, result.invocation_id);
    WriteId(writer, result.attempt_id);
    WriteModuleIdentity(
        writer,
        result.module,
        CanonicalHashMode::IncludeDeclaredHash);
    writer.String(result.entrypoint);
    WriteDependencyLock(writer, result.resolved_dependencies);
    writer.Enum(result.infrastructure);
    writer.Optional(result.domain_outcome, WriteProgramValueGraph);
    writer.Enum(result.cleanup);
    writer.Enum(result.session_disposition);
    writer.Optional(result.output, WriteProgramValueGraph);
    writer.Vector(result.emissions, WriteEmission);
    writer.Vector(result.artifacts, WriteArtifact);
    writer.Vector(result.diagnostics, WriteDiagnostic);
    writer.Vector(result.trace, WriteTraceEvent);
    writer.Vector(result.cleanup_receipts, WriteCleanupReceipt);
    WriteProvenance(writer, result.provenance);
}

bool ReadResultBody(Reader& reader, ProgramResult& result)
{
    return ReadId(reader, result.invocation_id) &&
        ReadId(reader, result.attempt_id) &&
        ReadModuleIdentity(reader, result.module) &&
        reader.String(result.entrypoint) &&
        ReadDependencyLock(reader, result.resolved_dependencies) &&
        reader.EnumValue(
            result.infrastructure,
            ProgramInfrastructureStatus::ContractFailed) &&
        reader.Optional(result.domain_outcome, ReadProgramValueGraph) &&
        reader.EnumValue(result.cleanup, ProgramCleanupStatus::Tainted) &&
        reader.EnumValue(
            result.session_disposition,
            SessionDisposition::Tainted) &&
        reader.Optional(result.output, ReadProgramValueGraph) &&
        reader.Vector(result.emissions, ReadEmission) &&
        reader.Vector(result.artifacts, ReadArtifact) &&
        reader.Vector(result.diagnostics, ReadDiagnostic) &&
        reader.Vector(result.trace, ReadTraceEvent) &&
        reader.Vector(result.cleanup_receipts, ReadCleanupReceipt) &&
        ReadProvenance(reader, result.provenance);
}

[[nodiscard]] bool AuthoritativeGraph(
    const ProgramValueGraph& graph) noexcept
{
    return std::ranges::none_of(
        graph.values,
        [](const ProgramValue& value) {
            const auto* artifact =
                std::get_if<ArtifactReferenceValue>(
                    &value.payload);
            return artifact && !artifact->complete;
        });
}

[[nodiscard]] bool AuthoritativeResult(
    const ProgramResult& result) noexcept
{
    if ((result.domain_outcome &&
         !AuthoritativeGraph(*result.domain_outcome)) ||
        (result.output && !AuthoritativeGraph(*result.output)) ||
        std::ranges::any_of(
            result.emissions,
            [](const ProgramEmission& emission) {
                return !emission.complete ||
                    !AuthoritativeGraph(emission.value);
            }) ||
        std::ranges::any_of(
            result.artifacts,
            [](const ProgramArtifact& artifact) {
                return !artifact.artifact.complete;
            }) ||
        std::ranges::any_of(
            result.provenance.source_artifacts,
            [](const ArtifactReferenceValue& artifact) {
                return !artifact.complete;
            }))
    {
        return false;
    }
    return true;
}

template <typename BodyWriter>
EncodeResult EncodeEnvelope(
    const std::array<Byte, 4>& magic,
    const CodecLimits& limits,
    BodyWriter&& body_writer)
{
    Writer body(limits);
    body_writer(body);
    if (!body.status())
        return {body.status(), {}};

    const auto& payload = body.bytes();
    if (payload.size() > limits.maximum_payload_bytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return {
            {CodecError::Oversized, "payload exceeds canonical envelope limit"},
            {},
        };
    }

    CodecLimits envelope_limits = limits;
    if (envelope_limits.maximum_payload_bytes <=
        std::numeric_limits<std::uint64_t>::max() - kProgramCodecHeaderSizeV1)
    {
        envelope_limits.maximum_payload_bytes += kProgramCodecHeaderSizeV1;
    }
    Writer envelope(envelope_limits);
    envelope.Raw(magic);
    envelope.U16(kProgramCodecVersionV1);
    envelope.U32(static_cast<std::uint32_t>(payload.size()));
    envelope.Raw(payload);
    return {envelope.status(), envelope.TakeBytes()};
}

struct EnvelopeBody
{
    CodecStatus status;
    std::span<const Byte> body;
    std::size_t consumed = 0;
};

EnvelopeBody DecodeEnvelope(
    std::span<const Byte> bytes,
    const std::array<Byte, 4>& expected_magic,
    const CodecLimits& limits)
{
    if (bytes.size() < kProgramCodecHeaderSizeV1)
    {
        return {
            {CodecError::Truncated, "canonical envelope header is truncated"},
            {},
            0,
        };
    }
    if (!std::ranges::equal(bytes.first<4>(), expected_magic))
    {
        return {
            {CodecError::InvalidMagic, "canonical envelope magic is invalid"},
            {},
            0,
        };
    }
    const std::uint16_t version =
        static_cast<std::uint16_t>(bytes[4]) |
        (static_cast<std::uint16_t>(bytes[5]) << 8);
    if (version != kProgramCodecVersionV1)
    {
        return {
            {CodecError::UnsupportedVersion, "canonical envelope version is unsupported"},
            {},
            0,
        };
    }
    const std::uint32_t length =
        static_cast<std::uint32_t>(bytes[6]) |
        (static_cast<std::uint32_t>(bytes[7]) << 8) |
        (static_cast<std::uint32_t>(bytes[8]) << 16) |
        (static_cast<std::uint32_t>(bytes[9]) << 24);
    if (length > limits.maximum_payload_bytes)
    {
        return {
            {CodecError::Oversized, "canonical envelope payload exceeds configured limit"},
            {},
            0,
        };
    }
    const std::size_t required =
        kProgramCodecHeaderSizeV1 + static_cast<std::size_t>(length);
    if (bytes.size() < required)
    {
        return {
            {CodecError::Truncated, "canonical envelope payload is truncated"},
            {},
            0,
        };
    }
    if (bytes.size() > required)
    {
        return {
            {CodecError::TrailingBytes, "canonical envelope has trailing bytes"},
            {},
            required,
        };
    }
    return {
        {},
        bytes.subspan(kProgramCodecHeaderSizeV1, length),
        required,
    };
}

template <typename T, typename BodyReader, typename Encoder>
DecodeResult<T> DecodeCanonical(
    std::span<const Byte> bytes,
    const std::array<Byte, 4>& magic,
    const CodecLimits& limits,
    BodyReader&& body_reader,
    Encoder&& encoder)
{
    const auto envelope = DecodeEnvelope(bytes, magic, limits);
    if (!envelope.status)
        return {envelope.status, std::nullopt, envelope.consumed};

    Reader reader(envelope.body, limits);
    T value{};
    if (!body_reader(reader, value))
        return {reader.status(), std::nullopt, envelope.consumed};
    if (!reader.at_end())
    {
        return {
            {CodecError::TrailingBytes, "canonical payload has trailing body bytes"},
            std::nullopt,
            envelope.consumed,
        };
    }

    const auto canonical = encoder(value, limits);
    if (!canonical)
        return {canonical.status, std::nullopt, envelope.consumed};
    if (canonical.bytes != std::vector<Byte>(bytes.begin(), bytes.end()))
    {
        return {
            {CodecError::NonCanonical, "payload does not use canonical ordering"},
            std::nullopt,
            envelope.consumed,
        };
    }
    return {{}, std::move(value), envelope.consumed};
}

template <typename T, typename Key>
bool HasDuplicate(const std::vector<T>& values, Key key)
{
    std::set<decltype(key(std::declval<const T&>()))> seen;
    for (const auto& value : values)
    {
        if (!seen.insert(key(value)).second)
            return true;
    }
    return false;
}

CodecStatus ValidateModuleShape(const ProgramModule& module)
{
    if (module.identity.canonical_id.empty() || module.identity.revision == 0)
        return {CodecError::InvalidValue, "module identity is incomplete"};
    if (module.ir_version != kCanonicalIrVersionV1)
        return {CodecError::UnsupportedVersion, "module IR version is unsupported"};
    if (HasDuplicate(module.entrypoints, [](const ProgramEntrypoint& value) {
            return value.name;
        }))
    {
        return {CodecError::InvalidValue, "module contains duplicate entrypoints"};
    }
    for (const auto& entrypoint : module.entrypoints)
    {
        if (entrypoint.name.empty() || !entrypoint.function)
        {
            return {
                CodecError::InvalidValue,
                "entrypoint identity is incomplete",
            };
        }
        if (HasDuplicate(
                entrypoint.emission_schemas,
                [](const SchemaIdentity& value) {
                    return std::pair(value.canonical_id, value.version);
                }) ||
            HasDuplicate(
                entrypoint.artifact_schemas,
                [](const SchemaIdentity& value) {
                    return std::pair(value.canonical_id, value.version);
                }) ||
            HasDuplicate(
                entrypoint.required_capability_packs,
                [](const CapabilityPackIdentity& value) {
                    return std::pair(value.canonical_id, value.version);
                }))
        {
            return {
                CodecError::InvalidValue,
                "entrypoint contains duplicate declarations",
            };
        }
    }
    if (HasDuplicate(module.functions, [](const ProgramFunction& value) {
            return value.id.value();
        }))
    {
        return {CodecError::InvalidValue, "module contains duplicate function IDs"};
    }
    if (HasDuplicate(module.local_types, [](const TypeSchemaDefinition& value) {
            return std::pair(value.identity.canonical_id, value.identity.version);
        }))
    {
        return {CodecError::InvalidValue, "module contains duplicate local types"};
    }
    for (const auto& type : module.local_types)
    {
        if (type.identity.canonical_id.empty() ||
            type.identity.version == 0 ||
            type.identity.schema_hash.empty())
        {
            return {
                CodecError::InvalidValue,
                "local type identity is incomplete",
            };
        }
    }
    auto duplicate_exact = [](const std::vector<ExactDependencyIdentity>& values) {
        return HasDuplicate(values, [](const ExactDependencyIdentity& value) {
            return std::pair(value.canonical_id, value.version);
        });
    };
    if (duplicate_exact(module.action_imports) ||
        duplicate_exact(module.reducer_imports))
    {
        return {CodecError::InvalidValue, "module contains duplicate exact dependencies"};
    }
    for (const auto& dependency : module.action_imports)
    {
        if (dependency.canonical_id.empty() ||
            dependency.version == 0 ||
            dependency.signature_hash.empty())
        {
            return {
                CodecError::InvalidValue,
                "action dependency identity is incomplete",
            };
        }
    }
    for (const auto& dependency : module.reducer_imports)
    {
        if (dependency.canonical_id.empty() ||
            dependency.version == 0 ||
            dependency.signature_hash.empty())
        {
            return {
                CodecError::InvalidValue,
                "reducer dependency identity is incomplete",
            };
        }
    }
    if (HasDuplicate(module.module_imports, [](const ModuleImportIdentity& value) {
            return std::pair(
                value.module.canonical_id,
                value.module.revision);
        }) ||
        HasDuplicate(module.type_imports, [](const SchemaIdentity& value) {
            return std::pair(value.canonical_id, value.version);
        }) ||
        HasDuplicate(
            module.required_capability_packs,
            [](const CapabilityPackIdentity& value) {
                return std::pair(value.canonical_id, value.version);
            }))
    {
        return {CodecError::InvalidValue, "module contains duplicate imports"};
    }
    if (HasDuplicate(module.source_map.entries, [](const SourceMapEntry& value) {
            return value.id.value();
        }))
    {
        return {
            CodecError::InvalidValue,
            "module contains duplicate source-map locations",
        };
    }
    for (const auto& function : module.functions)
    {
        if (!function.id || function.name.empty() || !function.entry_block)
            return {CodecError::InvalidValue, "function identity is incomplete"};
        if (HasDuplicate(function.blocks, [](const BasicBlock& value) {
                return value.id.value();
            }))
        {
            return {CodecError::InvalidValue, "function contains duplicate blocks"};
        }
        for (const auto& block : function.blocks)
        {
            if (!block.id)
                return {CodecError::InvalidValue, "block ID must be nonzero"};
            if (HasDuplicate(block.instructions, [](const Instruction& value) {
                    return value.id.value();
                }))
            {
                return {CodecError::InvalidValue, "block contains duplicate instructions"};
            }
        }
    }
    return {};
}

} // namespace

EncodeResult EncodeProgramModuleV1(
    const ProgramModule& module,
    CanonicalHashMode hash_mode,
    const CodecLimits& limits)
{
    const auto shape = ValidateModuleShape(module);
    if (!shape)
        return {shape, {}};
    return EncodeEnvelope(
        kModuleMagic,
        limits,
        [&](Writer& writer) { WriteModuleBody(writer, module, hash_mode); });
}

DecodeResult<ProgramModule> DecodeProgramModuleV1(
    std::span<const Byte> bytes,
    const CodecLimits& limits)
{
    auto result = DecodeCanonical<ProgramModule>(
        bytes,
        kModuleMagic,
        limits,
        ReadModuleBody,
        [](const ProgramModule& module, const CodecLimits& codec_limits) {
            return EncodeProgramModuleV1(
                module,
                CanonicalHashMode::IncludeDeclaredHash,
                codec_limits);
        });
    if (!result)
        return result;
    const auto identity = ValidateProgramModuleIdentityV1(*result.value);
    if (!identity)
        return {identity, std::nullopt, result.consumed};
    return result;
}

ContentHash256 ComputeProgramModuleHashV1(const ProgramModule& module)
{
    ContentHash256 result;
    const auto encoded = EncodeProgramModuleV1(
        module,
        CanonicalHashMode::OmitDeclaredHash);
    if (!encoded)
        return result;
    if (mbedtls_sha256_ret(
            encoded.bytes.data(),
            encoded.bytes.size(),
            result.bytes.data(),
            0) != 0)
    {
        return {};
    }
    return result;
}

ContentHash256 ComputeProgramDependencyLockHashV1(
    const ProgramDependencyLock& dependency_lock,
    const CodecLimits& limits)
{
    const EncodeResult encoded = EncodeEnvelope(
        kDependencyLockMagic,
        limits,
        [&](Writer& writer) {
            WriteDependencyLock(writer, dependency_lock);
        });
    if (!encoded)
        return {};

    ContentHash256 result;
    if (mbedtls_sha256_ret(
            encoded.bytes.data(),
            encoded.bytes.size(),
            result.bytes.data(),
            0) != 0)
    {
        return {};
    }
    return result;
}

CodecStatus ValidateProgramModuleIdentityV1(const ProgramModule& module)
{
    const auto shape = ValidateModuleShape(module);
    if (!shape)
        return shape;
    if (module.identity.module_hash.empty())
        return {CodecError::HashMismatch, "module hash is absent"};
    const auto actual = ComputeProgramModuleHashV1(module);
    if (actual.empty())
        return {CodecError::HashFailure, "module hash computation failed"};
    if (actual != module.identity.module_hash)
        return {CodecError::HashMismatch, "module hash does not match canonical bytes"};
    return {};
}

EncodeResult EncodeProgramInvocationV1(
    const ProgramInvocation& invocation,
    const CodecLimits& limits)
{
    return EncodeEnvelope(
        kInvocationMagic,
        limits,
        [&](Writer& writer) { WriteInvocationBody(writer, invocation); });
}

DecodeResult<ProgramInvocation> DecodeProgramInvocationV1(
    std::span<const Byte> bytes,
    const CodecLimits& limits)
{
    return DecodeCanonical<ProgramInvocation>(
        bytes,
        kInvocationMagic,
        limits,
        ReadInvocationBody,
        EncodeProgramInvocationV1);
}

EncodeResult EncodeProgramEmissionV1(
    const ProgramEmission& emission,
    const CodecLimits& limits)
{
    return EncodeEnvelope(
        kEmissionMagic,
        limits,
        [&](Writer& writer) { WriteEmission(writer, emission); });
}

DecodeResult<ProgramEmission> DecodeProgramEmissionV1(
    std::span<const Byte> bytes,
    const CodecLimits& limits)
{
    return DecodeCanonical<ProgramEmission>(
        bytes,
        kEmissionMagic,
        limits,
        ReadEmission,
        EncodeProgramEmissionV1);
}

EncodeResult EncodeProgramResultV1(
    const ProgramResult& result,
    const CodecLimits& limits)
{
    if (!AuthoritativeResult(result))
    {
        return {
            {CodecError::InvalidValue,
             "program result contains an incomplete internal artifact"},
            {}};
    }
    return EncodeEnvelope(
        kResultMagic,
        limits,
        [&](Writer& writer) { WriteResultBody(writer, result); });
}

DecodeResult<ProgramResult> DecodeProgramResultV1(
    std::span<const Byte> bytes,
    const CodecLimits& limits)
{
    return DecodeCanonical<ProgramResult>(
        bytes,
        kResultMagic,
        limits,
        ReadResultBody,
        EncodeProgramResultV1);
}

} // namespace savor::runtime::program
