#pragma once

#include "ProgramIdentifiers.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace savor::runtime::program {

enum class SemanticPointKind : std::uint8_t
{
    ProgramCounter = 0,
    Memory = 1,
    Synthetic = 2,
};

enum class BuiltinType : std::uint8_t
{
    Unit,
    Bool,
    U8,
    U16,
    U32,
    U64,
    I32,
    I64,
    F32,
    F64,
};

struct TypeRef
{
    BuiltinType builtin = BuiltinType::Unit;
    std::optional<SchemaIdentity> named;

    [[nodiscard]] static TypeRef Builtin(BuiltinType type) noexcept;
    [[nodiscard]] static TypeRef Named(SchemaIdentity identity);
    [[nodiscard]] bool is_named() const noexcept { return named.has_value(); }

    auto operator<=>(const TypeRef&) const = default;
};

enum class TypeSchemaKind : std::uint8_t
{
    BoundedUtf8String,
    BoundedBytes,
    ClosedEnum,
    Optional,
    Record,
    BoundedList,
    ArtifactReference,
    ResourceHandle,
    EpochBoundOpaqueHandle,
};

struct EnumMemberDefinition
{
    std::string name;
    std::int64_t value = 0;

    auto operator<=>(const EnumMemberDefinition&) const = default;
};

struct RecordFieldDefinition
{
    std::string name;
    TypeRef type;

    auto operator<=>(const RecordFieldDefinition&) const = default;
};

struct TypeSchemaDefinition
{
    SchemaIdentity identity;
    TypeSchemaKind kind = TypeSchemaKind::Record;
    std::uint64_t maximum_size = 0;
    std::optional<TypeRef> element_type;
    std::vector<EnumMemberDefinition> enum_members;
    std::vector<RecordFieldDefinition> record_fields;
    bool permits_incomplete = false;

    auto operator<=>(const TypeSchemaDefinition&) const = default;
};

struct UnitValue
{
    auto operator<=>(const UnitValue&) const = default;
};

struct EnumValue
{
    SchemaIdentity schema;
    std::int64_t value = 0;

    auto operator<=>(const EnumValue&) const = default;
};

struct OptionalValue
{
    std::optional<ProgramValueId> value;

    auto operator<=>(const OptionalValue&) const = default;
};

struct RecordValue
{
    std::vector<ProgramValueId> fields;

    auto operator<=>(const RecordValue&) const = default;
};

struct ListValue
{
    std::vector<ProgramValueId> elements;

    auto operator<=>(const ListValue&) const = default;
};

struct ArtifactReferenceValue
{
    std::string artifact_id;
    SchemaIdentity schema;
    ContentHash256 content_hash;
    std::string storage_reference;
    bool complete = true;

    auto operator<=>(const ArtifactReferenceValue&) const = default;
};

struct ResourceHandleValue
{
    ProgramResourceHandleId handle_id;
    SchemaIdentity resource_type;
    WorksetEpoch workset_epoch;

    auto operator<=>(const ResourceHandleValue&) const = default;
};

struct OpaqueHandleValue
{
    ProgramResourceHandleId handle_id;
    SchemaIdentity handle_type;
    WorksetEpoch workset_epoch;

    auto operator<=>(const OpaqueHandleValue&) const = default;
};

using LiteralPayload = std::variant<
    UnitValue,
    bool,
    std::uint8_t,
    std::uint16_t,
    std::uint32_t,
    std::uint64_t,
    std::int32_t,
    std::int64_t,
    float,
    double,
    std::string,
    std::vector<Byte>,
    EnumValue>;

struct LiteralValue
{
    TypeRef type;
    LiteralPayload payload;

    auto operator<=>(const LiteralValue&) const = default;
};

using ProgramValuePayload = std::variant<
    UnitValue,
    bool,
    std::uint8_t,
    std::uint16_t,
    std::uint32_t,
    std::uint64_t,
    std::int32_t,
    std::int64_t,
    float,
    double,
    std::string,
    std::vector<Byte>,
    EnumValue,
    OptionalValue,
    RecordValue,
    ListValue,
    ArtifactReferenceValue,
    ResourceHandleValue,
    OpaqueHandleValue>;

struct ProgramValue
{
    ProgramValueId id;
    TypeRef type;
    ProgramValuePayload payload;

    auto operator<=>(const ProgramValue&) const = default;
};

// A closed immutable value arena projection. Every composite ProgramValueId
// reachable from root must resolve exactly once within values.
struct ProgramValueGraph
{
    ProgramValueId root;
    std::vector<ProgramValue> values;

    auto operator<=>(const ProgramValueGraph&) const = default;
};

} // namespace savor::runtime::program
