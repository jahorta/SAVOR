#include "TypeSchemaRegistry.h"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

namespace savor::runtime::program {
namespace {

using Key = std::pair<std::string, std::uint32_t>;

Key MakeKey(const SchemaIdentity& identity)
{
    return {identity.canonical_id, identity.version};
}

RegistryResult Failure(
    RegistryErrorCode code,
    const std::string& message)
{
    return RegistryResult::Failure(code, message);
}

bool IsValidNamedRef(const TypeRef& type)
{
    return !type.is_named() ||
        (!type.named->canonical_id.empty() &&
         type.named->version != 0 &&
         !type.named->schema_hash.empty());
}

std::vector<SchemaIdentity> Dependencies(
    const TypeSchemaDefinition& definition)
{
    std::vector<SchemaIdentity> result;
    if (definition.element_type && definition.element_type->is_named())
        result.push_back(*definition.element_type->named);
    for (const RecordFieldDefinition& field : definition.record_fields)
    {
        if (field.type.is_named())
            result.push_back(*field.type.named);
    }
    return result;
}

} // namespace

RegistryResult TypeSchemaRegistry::Register(
    TypeSchemaDefinition definition)
{
    std::vector<TypeSchemaDefinition> batch;
    batch.push_back(std::move(definition));
    return RegisterBatch(std::move(batch));
}

RegistryResult TypeSchemaRegistry::RegisterBatch(
    std::vector<TypeSchemaDefinition> definitions)
{
    DefinitionMap candidate = definitions_;
    for (TypeSchemaDefinition& definition : definitions)
    {
        const RegistryResult shape = ValidateShape(definition);
        if (!shape.success)
            return shape;

        const Key key = MakeKey(definition.identity);
        const auto existing = candidate.find(key);
        if (existing != candidate.end())
        {
            if (existing->second == definition)
                continue;
            return Failure(
                RegistryErrorCode::IdentityConflict,
                "Schema identity already has different content: " +
                    definition.identity.canonical_id);
        }
        candidate.emplace(key, std::move(definition));
    }

    const RegistryResult validation = ValidateCandidate(candidate);
    if (!validation.success)
        return validation;

    definitions_ = std::move(candidate);
    return RegistryResult::Success();
}

const TypeSchemaDefinition* TypeSchemaRegistry::Resolve(
    const SchemaIdentity& identity) const noexcept
{
    const TypeSchemaDefinition* definition =
        Resolve(identity.canonical_id, identity.version);
    if (!definition ||
        definition->identity.schema_hash != identity.schema_hash)
    {
        return nullptr;
    }
    return definition;
}

const TypeSchemaDefinition* TypeSchemaRegistry::Resolve(
    const std::string& canonical_id,
    std::uint32_t version) const noexcept
{
    const auto found = definitions_.find({canonical_id, version});
    return found == definitions_.end() ? nullptr : &found->second;
}

std::optional<std::vector<TypeSchemaDefinition>>
TypeSchemaRegistry::ResolveClosure(
    const std::vector<SchemaIdentity>& roots,
    RegistryError* error) const
{
    enum class Visit : std::uint8_t
    {
        Visiting,
        Complete,
    };
    std::map<Key, Visit> visits;
    std::vector<TypeSchemaDefinition> result;

    std::function<bool(const SchemaIdentity&)> visit =
        [&](const SchemaIdentity& identity) -> bool {
        const TypeSchemaDefinition* definition = Resolve(identity);
        if (!definition)
        {
            if (error)
            {
                *error = {
                    RegistryErrorCode::DependencyMissing,
                    "Exact schema dependency is not registered: " +
                        identity.canonical_id,
                };
            }
            return false;
        }

        const Key key = MakeKey(identity);
        const auto prior = visits.find(key);
        if (prior != visits.end())
        {
            if (prior->second == Visit::Visiting)
            {
                if (error)
                {
                    *error = {
                        RegistryErrorCode::DependencyCycle,
                        "Recursive schema dependency: " +
                            identity.canonical_id,
                    };
                }
                return false;
            }
            return true;
        }

        visits.emplace(key, Visit::Visiting);
        for (const SchemaIdentity& dependency : Dependencies(*definition))
        {
            if (!visit(dependency))
                return false;
        }
        visits[key] = Visit::Complete;
        result.push_back(*definition);
        return true;
    };

    for (const SchemaIdentity& root : roots)
    {
        if (!visit(root))
            return std::nullopt;
    }
    if (error)
        *error = {};
    return result;
}

RegistryResult TypeSchemaRegistry::ValidateShape(
    const TypeSchemaDefinition& definition)
{
    if (definition.identity.canonical_id.empty() ||
        definition.identity.version == 0 ||
        definition.identity.schema_hash.empty())
    {
        return Failure(
            RegistryErrorCode::InvalidIdentity,
            "Schema identity requires canonical id, nonzero version, and hash");
    }

    std::set<std::string> field_names;
    for (const RecordFieldDefinition& field : definition.record_fields)
    {
        if (field.name.empty() || !field_names.emplace(field.name).second)
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Record fields require unique nonempty names");
        }
        if (!IsValidNamedRef(field.type))
        {
            return Failure(
                RegistryErrorCode::InvalidIdentity,
                "Record field has an invalid named schema reference");
        }
    }
    if (definition.element_type &&
        !IsValidNamedRef(*definition.element_type))
    {
        return Failure(
            RegistryErrorCode::InvalidIdentity,
            "Schema has an invalid element schema reference");
    }

    std::set<std::string> enum_names;
    std::set<std::int64_t> enum_values;
    for (const EnumMemberDefinition& member : definition.enum_members)
    {
        if (member.name.empty() ||
            !enum_names.emplace(member.name).second ||
            !enum_values.emplace(member.value).second)
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Enum members require unique nonempty names and values");
        }
    }

    switch (definition.kind)
    {
    case TypeSchemaKind::BoundedUtf8String:
    case TypeSchemaKind::BoundedBytes:
        if (definition.maximum_size == 0 ||
            definition.element_type ||
            !definition.enum_members.empty() ||
            !definition.record_fields.empty())
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Bounded string/bytes schema has invalid shape");
        }
        break;
    case TypeSchemaKind::ClosedEnum:
        if (definition.enum_members.empty() ||
            definition.element_type ||
            !definition.record_fields.empty())
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Closed enum requires members and no element or fields");
        }
        break;
    case TypeSchemaKind::Optional:
        if (!definition.element_type ||
            !definition.enum_members.empty() ||
            !definition.record_fields.empty())
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Optional schema requires exactly one element type");
        }
        break;
    case TypeSchemaKind::Record:
        if (definition.element_type || !definition.enum_members.empty())
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Record schema cannot have an element or enum members");
        }
        break;
    case TypeSchemaKind::BoundedList:
        if (definition.maximum_size == 0 ||
            !definition.element_type ||
            !definition.enum_members.empty() ||
            !definition.record_fields.empty())
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Bounded list requires a nonzero bound and element type");
        }
        break;
    case TypeSchemaKind::ArtifactReference:
    case TypeSchemaKind::ResourceHandle:
    case TypeSchemaKind::EpochBoundOpaqueHandle:
        if (!definition.element_type ||
            !definition.element_type->is_named() ||
            !definition.enum_members.empty() ||
            !definition.record_fields.empty())
        {
            return Failure(
                RegistryErrorCode::InvalidArgument,
                "Typed reference/handle requires an element type");
        }
        break;
    }

    return RegistryResult::Success();
}

RegistryResult TypeSchemaRegistry::ValidateCandidate(
    const DefinitionMap& definitions)
{
    enum class Visit : std::uint8_t
    {
        Visiting,
        Complete,
    };
    std::map<Key, Visit> visits;

    std::function<RegistryResult(const TypeSchemaDefinition&)> visit =
        [&](const TypeSchemaDefinition& definition) -> RegistryResult {
        const Key key = MakeKey(definition.identity);
        const auto prior = visits.find(key);
        if (prior != visits.end())
        {
            if (prior->second == Visit::Visiting)
            {
                return Failure(
                    RegistryErrorCode::DependencyCycle,
                    "Recursive schema definitions are forbidden: " +
                        definition.identity.canonical_id);
            }
            return RegistryResult::Success();
        }

        visits.emplace(key, Visit::Visiting);
        for (const SchemaIdentity& dependency : Dependencies(definition))
        {
            const auto found = definitions.find(MakeKey(dependency));
            if (found == definitions.end())
            {
                return Failure(
                    RegistryErrorCode::DependencyMissing,
                    "Schema dependency is not registered: " +
                        dependency.canonical_id);
            }
            if (found->second.identity.schema_hash != dependency.schema_hash)
            {
                return Failure(
                    RegistryErrorCode::ReferenceMismatch,
                    "Schema dependency hash mismatch: " +
                        dependency.canonical_id);
            }
            const RegistryResult nested = visit(found->second);
            if (!nested.success)
                return nested;
        }
        visits[key] = Visit::Complete;
        return RegistryResult::Success();
    };

    for (const auto& [key, definition] : definitions)
    {
        (void)key;
        const RegistryResult result = visit(definition);
        if (!result.success)
            return result;
    }
    return RegistryResult::Success();
}

} // namespace savor::runtime::program
