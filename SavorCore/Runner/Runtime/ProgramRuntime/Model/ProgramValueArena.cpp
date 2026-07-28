#include "ProgramValueArena.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace savor::runtime::program {
namespace {

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
        std::size_t count = 0;
        if ((lead & 0xe0) == 0xc0)
        {
            code_point = lead & 0x1f;
            count = 1;
            if (code_point == 0)
                return false;
        }
        else if ((lead & 0xf0) == 0xe0)
        {
            code_point = lead & 0x0f;
            count = 2;
        }
        else if ((lead & 0xf8) == 0xf0)
        {
            code_point = lead & 0x07;
            count = 3;
        }
        else
        {
            return false;
        }

        if (static_cast<std::size_t>(end - cursor) < count)
            return false;
        for (std::size_t index = 0; index < count; ++index)
        {
            const unsigned char continuation = *cursor++;
            if ((continuation & 0xc0) != 0x80)
                return false;
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if ((count == 1 && code_point < 0x80) ||
            (count == 2 && code_point < 0x800) ||
            (count == 3 && code_point < 0x10000) ||
            code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}

ProgramValueArenaStatus ValidatePayloadSemantics(
    const ProgramValuePayload& payload)
{
    return std::visit(
        [](const auto& value) -> ProgramValueArenaStatus {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                if (!IsValidUtf8(value))
                {
                    return {
                        ProgramValueArenaError::InvalidUtf8,
                        "program string value is not valid UTF-8",
                    };
                }
            }
            else if constexpr (std::is_same_v<T, float> ||
                               std::is_same_v<T, double>)
            {
                if (!std::isfinite(value))
                {
                    return {
                        ProgramValueArenaError::NonFiniteValue,
                        "program floating-point value must be finite",
                    };
                }
            }
            return {};
        },
        payload);
}

template <typename Function>
bool ForEachChild(const ProgramValuePayload& payload, Function&& function)
{
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, OptionalValue>)
            {
                return !value.value || function(*value.value);
            }
            else if constexpr (std::is_same_v<T, RecordValue>)
            {
                return std::ranges::all_of(value.fields, function);
            }
            else if constexpr (std::is_same_v<T, ListValue>)
            {
                return std::ranges::all_of(value.elements, function);
            }
            else
            {
                return true;
            }
        },
        payload);
}

ProgramValuePayload RemapPayload(
    const ProgramValuePayload& payload,
    const std::map<ProgramValueId, ProgramValueId>& remap)
{
    return std::visit(
        [&](const auto& value) -> ProgramValuePayload {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, OptionalValue>)
            {
                OptionalValue result;
                if (value.value)
                    result.value = remap.at(*value.value);
                return result;
            }
            else if constexpr (std::is_same_v<T, RecordValue>)
            {
                RecordValue result;
                result.fields.reserve(value.fields.size());
                for (const auto id : value.fields)
                    result.fields.push_back(remap.at(id));
                return result;
            }
            else if constexpr (std::is_same_v<T, ListValue>)
            {
                ListValue result;
                result.elements.reserve(value.elements.size());
                for (const auto id : value.elements)
                    result.elements.push_back(remap.at(id));
                return result;
            }
            else
            {
                return value;
            }
        },
        payload);
}

std::uint64_t IdentityBytes(const SchemaIdentity& identity) noexcept
{
    return static_cast<std::uint64_t>(identity.canonical_id.size()) +
        sizeof(identity.version) + identity.schema_hash.bytes.size();
}

} // namespace

ProgramValueArena::ProgramValueArena(
    ProgramValueArenaLimits limits) noexcept
    : limits_(limits)
{
}

ProgramValueArenaInsertResult ProgramValueArena::Add(
    TypeRef type,
    ProgramValuePayload payload)
{
    const auto semantics = ValidatePayloadSemantics(payload);
    if (!semantics)
        return {semantics, {}};
    if (values_.size() >= limits_.maximum_values)
    {
        return {
            {ProgramValueArenaError::ValueBudgetExceeded,
             "program value count budget is exhausted"},
            {},
        };
    }
    const auto references = ValidateReferences(payload);
    if (!references)
        return {references, {}};

    const std::uint64_t bytes = Measure(type, payload);
    if (bytes > limits_.maximum_value_bytes ||
        value_bytes_ > limits_.maximum_value_bytes - bytes)
    {
        return {
            {ProgramValueArenaError::ByteBudgetExceeded,
             "program value byte budget is exhausted"},
            {},
        };
    }
    if (next_id_ == 0)
    {
        return {
            {ProgramValueArenaError::ValueBudgetExceeded,
             "program value identity space is exhausted"},
            {},
        };
    }

    const ProgramValueId id(next_id_++);
    values_.emplace(
        id,
        ProgramValue{
            .id = id,
            .type = std::move(type),
            .payload = std::move(payload),
        });
    value_bytes_ += bytes;
    return {{}, id};
}

ProgramValueArenaInsertResult ProgramValueArena::Clone(
    ProgramValueId source)
{
    const ProgramValue* value = Lookup(source);
    if (!value)
    {
        return {
            {ProgramValueArenaError::InvalidId,
             "clone source is not present in the arena"},
            {},
        };
    }
    return Add(value->type, value->payload);
}

ProgramValueArenaInsertResult ProgramValueArena::CloneWithPayload(
    ProgramValueId source,
    ProgramValuePayload replacement)
{
    const ProgramValue* value = Lookup(source);
    if (!value)
    {
        return {
            {ProgramValueArenaError::InvalidId,
             "clone source is not present in the arena"},
            {},
        };
    }
    return Add(value->type, std::move(replacement));
}

ProgramValueArenaInsertResult ProgramValueArena::Import(
    const ProgramValueGraph& graph)
{
    if (!graph.root)
    {
        return {
            {ProgramValueArenaError::InvalidId,
             "program value graph root must be nonzero"},
            {},
        };
    }

    std::map<ProgramValueId, const ProgramValue*> source;
    for (const auto& value : graph.values)
    {
        if (!value.id)
        {
            return {
                {ProgramValueArenaError::InvalidId,
                 "program value graph contains a zero ID"},
                {},
            };
        }
        if (!source.emplace(value.id, &value).second)
        {
            return {
                {ProgramValueArenaError::DuplicateId,
                 "program value graph contains duplicate IDs"},
                {},
            };
        }
    }
    if (!source.contains(graph.root))
    {
        return {
            {ProgramValueArenaError::UnresolvedReference,
             "program value graph root is unresolved"},
            {},
        };
    }

    std::map<ProgramValueId, std::uint8_t> state;
    std::vector<ProgramValueId> order;
    std::function<ProgramValueArenaStatus(ProgramValueId)> visit =
        [&](ProgramValueId id) -> ProgramValueArenaStatus {
        const auto found = source.find(id);
        if (found == source.end())
        {
            return {
                ProgramValueArenaError::UnresolvedReference,
                "program value graph contains an unresolved child ID",
            };
        }
        if (state[id] == 1)
        {
            return {
                ProgramValueArenaError::CyclicGraph,
                "program value graph contains a cycle",
            };
        }
        if (state[id] == 2)
            return {};
        state[id] = 1;
        ProgramValueArenaStatus child_status;
        const bool complete = ForEachChild(
            found->second->payload,
            [&](ProgramValueId child) {
                child_status = visit(child);
                return static_cast<bool>(child_status);
            });
        if (!complete)
            return child_status;
        state[id] = 2;
        order.push_back(id);
        return {};
    };

    const auto graph_status = visit(graph.root);
    if (!graph_status)
        return {graph_status, {}};
    if (order.size() != source.size())
    {
        return {
            {ProgramValueArenaError::UnreachableValue,
             "program value graph contains unreachable values"},
            {},
        };
    }
    if (order.size() > limits_.maximum_values - values_.size())
    {
        return {
            {ProgramValueArenaError::ValueBudgetExceeded,
             "program value graph exceeds the remaining value budget"},
            {},
        };
    }
    if (order.size() >
        std::numeric_limits<std::uint64_t>::max() - next_id_)
    {
        return {
            {ProgramValueArenaError::ValueBudgetExceeded,
             "program value identity space cannot fit the graph"},
            {},
        };
    }

    std::uint64_t required_bytes = 0;
    for (const auto id : order)
    {
        const auto& value = *source.at(id);
        const auto semantics = ValidatePayloadSemantics(value.payload);
        if (!semantics)
            return {semantics, {}};
        const std::uint64_t bytes = Measure(value.type, value.payload);
        if (bytes > limits_.maximum_value_bytes ||
            required_bytes > limits_.maximum_value_bytes - bytes)
        {
            return {
                {ProgramValueArenaError::ByteBudgetExceeded,
                 "program value graph exceeds the byte budget"},
                {},
            };
        }
        required_bytes += bytes;
    }
    if (required_bytes > limits_.maximum_value_bytes ||
        value_bytes_ > limits_.maximum_value_bytes - required_bytes)
    {
        return {
            {ProgramValueArenaError::ByteBudgetExceeded,
             "program value graph exceeds the remaining byte budget"},
            {},
        };
    }

    std::map<ProgramValueId, ProgramValueId> remap;
    for (const auto source_id : order)
    {
        const auto& value = *source.at(source_id);
        const auto inserted = Add(
            value.type,
            RemapPayload(value.payload, remap));
        if (!inserted)
            return inserted;
        remap.emplace(source_id, inserted.value);
    }
    return {{}, remap.at(graph.root)};
}

ProgramValueArenaExportResult ProgramValueArena::Export(
    ProgramValueId root) const
{
    if (!Lookup(root))
    {
        return {
            {ProgramValueArenaError::InvalidId,
             "program value root is not present in the arena"},
            std::nullopt,
        };
    }

    std::set<ProgramValueId> included;
    std::function<void(ProgramValueId)> collect =
        [&](ProgramValueId id) {
        if (!included.insert(id).second)
            return;
        const ProgramValue* value = Lookup(id);
        if (!value)
            return;
        ForEachChild(
            value->payload,
            [&](ProgramValueId child) {
                collect(child);
                return true;
            });
    };
    collect(root);

    ProgramValueGraph graph{.root = root};
    graph.values.reserve(included.size());
    for (const auto id : included)
        graph.values.push_back(values_.at(id));
    return {{}, std::move(graph)};
}

const ProgramValue* ProgramValueArena::Lookup(
    ProgramValueId id) const noexcept
{
    const auto found = values_.find(id);
    return found == values_.end() ? nullptr : &found->second;
}

ProgramValueArenaLookupResult ProgramValueArena::LookupForEpoch(
    ProgramValueId id,
    StateEpoch current_epoch) const noexcept
{
    const ProgramValue* value = Lookup(id);
    if (!value)
    {
        return {
            {ProgramValueArenaError::InvalidId,
             "program value is not present in the arena"},
            nullptr,
        };
    }
    const bool stale = std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ResourceHandleValue>)
            {
                return payload.origin_epoch.has_value() &&
                    *payload.origin_epoch != current_epoch;
            }
            else if constexpr (std::is_same_v<T, OpaqueHandleValue>)
            {
                return payload.origin_epoch != current_epoch;
            }
            else
            {
                return false;
            }
        },
        value->payload);
    if (stale)
    {
        return {
            {ProgramValueArenaError::StaleEpoch,
             "epoch-bound program value belongs to a replaced state"},
            nullptr,
        };
    }
    return {{}, value};
}

ProgramValueGraphMeasureResult MeasureProgramValueGraph(
    const ProgramValueGraph& graph,
    ProgramValueArenaLimits limits)
{
    ProgramValueArena arena(limits);
    const ProgramValueArenaInsertResult imported =
        arena.Import(graph);
    if (!imported)
        return {imported.status, 0, 0};
    return {
        {},
        arena.value_count(),
        arena.value_bytes(),
    };
}

ProgramValueArenaStatus ValidateProgramValueGraph(
    const ProgramValueGraph& graph,
    const TypeRef& expected_root_type,
    std::span<const TypeSchemaDefinition> schemas,
    ProgramValueArenaLimits limits,
    std::optional<StateEpoch> current_epoch)
{
    ProgramValueArena arena(limits);
    const ProgramValueArenaInsertResult imported =
        arena.Import(graph);
    if (!imported)
        return imported.status;

    const ProgramValue* root = nullptr;
    std::map<ProgramValueId, const ProgramValue*> values;
    for (const ProgramValue& value : graph.values)
    {
        values.emplace(value.id, &value);
        if (value.id == graph.root)
            root = &value;
    }
    if (!root || root->type != expected_root_type)
    {
        return {
            ProgramValueArenaError::TypeMismatch,
            "program value graph root does not match its exact type"};
    }

    const auto schema_for =
        [schemas](const SchemaIdentity& identity)
            -> const TypeSchemaDefinition* {
        const auto found = std::ranges::find(
            schemas,
            identity,
            &TypeSchemaDefinition::identity);
        return found == schemas.end() ? nullptr : &*found;
    };
    const auto child =
        [&values](ProgramValueId id) -> const ProgramValue* {
        const auto found = values.find(id);
        return found == values.end() ? nullptr : found->second;
    };
    const auto type_failure = [] {
        return ProgramValueArenaStatus{
            ProgramValueArenaError::TypeMismatch,
            "program value payload does not match its declared type"};
    };
    const auto schema_failure = [](std::string message) {
        return ProgramValueArenaStatus{
            ProgramValueArenaError::SchemaMismatch,
            std::move(message)};
    };

    for (const ProgramValue& value : graph.values)
    {
        if (!value.type.is_named())
        {
            bool matches = false;
            switch (value.type.builtin)
            {
            case BuiltinType::Unit:
                matches =
                    std::holds_alternative<UnitValue>(value.payload);
                break;
            case BuiltinType::Bool:
                matches = std::holds_alternative<bool>(value.payload);
                break;
            case BuiltinType::U8:
                matches =
                    std::holds_alternative<std::uint8_t>(value.payload);
                break;
            case BuiltinType::U16:
                matches =
                    std::holds_alternative<std::uint16_t>(value.payload);
                break;
            case BuiltinType::U32:
                matches =
                    std::holds_alternative<std::uint32_t>(value.payload);
                break;
            case BuiltinType::U64:
                matches =
                    std::holds_alternative<std::uint64_t>(value.payload);
                break;
            case BuiltinType::I32:
                matches =
                    std::holds_alternative<std::int32_t>(value.payload);
                break;
            case BuiltinType::I64:
                matches =
                    std::holds_alternative<std::int64_t>(value.payload);
                break;
            case BuiltinType::F32:
                matches = std::holds_alternative<float>(value.payload);
                break;
            case BuiltinType::F64:
                matches = std::holds_alternative<double>(value.payload);
                break;
            }
            if (!matches)
                return type_failure();
            continue;
        }

        const TypeSchemaDefinition* schema =
            schema_for(*value.type.named);
        if (!schema)
        {
            return schema_failure(
                "program value uses a schema outside its verified closure");
        }
        switch (schema->kind)
        {
        case TypeSchemaKind::BoundedUtf8String:
        {
            const auto* text =
                std::get_if<std::string>(&value.payload);
            if (!text || text->size() > schema->maximum_size)
                return type_failure();
            break;
        }
        case TypeSchemaKind::BoundedBytes:
        {
            const auto* bytes =
                std::get_if<std::vector<Byte>>(&value.payload);
            if (!bytes || bytes->size() > schema->maximum_size)
                return type_failure();
            break;
        }
        case TypeSchemaKind::ClosedEnum:
        {
            const auto* enumeration =
                std::get_if<EnumValue>(&value.payload);
            if (!enumeration ||
                enumeration->schema != schema->identity ||
                !std::ranges::contains(
                    schema->enum_members,
                    enumeration->value,
                    &EnumMemberDefinition::value))
            {
                return schema_failure(
                    "program enum value is outside its closed schema");
            }
            break;
        }
        case TypeSchemaKind::Optional:
        {
            const auto* optional =
                std::get_if<OptionalValue>(&value.payload);
            if (!optional || !schema->element_type)
                return type_failure();
            if (optional->value)
            {
                const ProgramValue* item = child(*optional->value);
                if (!item || item->type != *schema->element_type)
                    return type_failure();
            }
            break;
        }
        case TypeSchemaKind::Record:
        {
            const auto* record =
                std::get_if<RecordValue>(&value.payload);
            if (!record ||
                record->fields.size() !=
                    schema->record_fields.size())
            {
                return type_failure();
            }
            for (std::size_t index = 0;
                 index < record->fields.size();
                 ++index)
            {
                const ProgramValue* field =
                    child(record->fields[index]);
                if (!field ||
                    field->type !=
                        schema->record_fields[index].type)
                {
                    return type_failure();
                }
            }
            break;
        }
        case TypeSchemaKind::BoundedList:
        {
            const auto* list =
                std::get_if<ListValue>(&value.payload);
            if (!list || !schema->element_type ||
                list->elements.size() > schema->maximum_size)
            {
                return type_failure();
            }
            for (ProgramValueId element : list->elements)
            {
                const ProgramValue* item = child(element);
                if (!item || item->type != *schema->element_type)
                    return type_failure();
            }
            break;
        }
        case TypeSchemaKind::ArtifactReference:
        {
            const auto* artifact =
                std::get_if<ArtifactReferenceValue>(&value.payload);
            if (!artifact || !schema->element_type ||
                !schema->element_type->named ||
                artifact->schema !=
                    *schema->element_type->named ||
                artifact->artifact_id.empty() ||
                artifact->content_hash.empty() ||
                artifact->storage_reference.empty() ||
                (!artifact->complete &&
                 !schema->permits_incomplete))
            {
                return schema_failure(
                    "artifact reference violates its exact schema");
            }
            break;
        }
        case TypeSchemaKind::ResourceHandle:
        {
            const auto* handle =
                std::get_if<ResourceHandleValue>(&value.payload);
            if (!handle || !handle->handle_id ||
                !schema->element_type ||
                !schema->element_type->named ||
                handle->resource_type !=
                    *schema->element_type->named ||
                (handle->origin_epoch &&
                 (!*handle->origin_epoch ||
                  (current_epoch &&
                   *handle->origin_epoch != *current_epoch))))
            {
                return schema_failure(
                    "resource handle violates its exact schema or epoch");
            }
            break;
        }
        case TypeSchemaKind::EpochBoundOpaqueHandle:
        {
            const auto* handle =
                std::get_if<OpaqueHandleValue>(&value.payload);
            if (!handle || !handle->handle_id ||
                !handle->origin_epoch ||
                !schema->element_type ||
                !schema->element_type->named ||
                handle->handle_type !=
                    *schema->element_type->named ||
                (current_epoch &&
                 handle->origin_epoch != *current_epoch))
            {
                return schema_failure(
                    "opaque handle violates its exact schema or epoch");
            }
            break;
        }
        }
    }
    return {};
}

ProgramValueArenaStatus ProgramValueArena::ValidateReferences(
    const ProgramValuePayload& payload) const
{
    const bool valid = ForEachChild(
        payload,
        [&](ProgramValueId child) {
            return static_cast<bool>(child) && values_.contains(child);
        });
    if (!valid)
    {
        return {
            ProgramValueArenaError::UnresolvedReference,
            "program value references an unknown arena value",
        };
    }
    return {};
}

std::uint64_t ProgramValueArena::Measure(
    const TypeRef& type,
    const ProgramValuePayload& payload) const noexcept
{
    std::uint64_t bytes = sizeof(ProgramValueId) + sizeof(BuiltinType);
    if (type.named)
        bytes += IdentityBytes(*type.named);
    bytes += std::visit(
        [](const auto& value) -> std::uint64_t {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, UnitValue>)
                return 0;
            else if constexpr (std::is_arithmetic_v<T>)
                return sizeof(T);
            else if constexpr (std::is_same_v<T, std::string> ||
                               std::is_same_v<T, std::vector<Byte>>)
                return value.size();
            else if constexpr (std::is_same_v<T, EnumValue>)
                return sizeof(value.value) + IdentityBytes(value.schema);
            else if constexpr (std::is_same_v<T, OptionalValue>)
                return sizeof(ProgramValueId);
            else if constexpr (std::is_same_v<T, RecordValue>)
                return value.fields.size() * sizeof(ProgramValueId);
            else if constexpr (std::is_same_v<T, ListValue>)
                return value.elements.size() * sizeof(ProgramValueId);
            else if constexpr (std::is_same_v<T, ArtifactReferenceValue>)
                return value.artifact_id.size() +
                    IdentityBytes(value.schema) +
                    value.content_hash.bytes.size() +
                    value.storage_reference.size() +
                    sizeof(value.complete);
            else if constexpr (std::is_same_v<T, ResourceHandleValue>)
                return sizeof(value.handle_id) +
                    IdentityBytes(value.resource_type) +
                    sizeof(StateEpoch);
            else if constexpr (std::is_same_v<T, OpaqueHandleValue>)
                return sizeof(value.handle_id) +
                    IdentityBytes(value.handle_type) +
                    sizeof(value.origin_epoch);
            else
                return 0;
        },
        payload);
    return bytes;
}

} // namespace savor::runtime::program
