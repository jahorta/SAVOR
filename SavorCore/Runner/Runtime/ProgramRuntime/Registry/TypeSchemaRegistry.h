#pragma once

#include "RegistryResult.h"
#include "Runner/Runtime/ProgramRuntime/Model/ProgramTypes.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime::program {

class TypeSchemaRegistry final
{
public:
    [[nodiscard]] RegistryResult Register(
        TypeSchemaDefinition definition);
    [[nodiscard]] RegistryResult RegisterBatch(
        std::vector<TypeSchemaDefinition> definitions);

    [[nodiscard]] const TypeSchemaDefinition* Resolve(
        const SchemaIdentity& identity) const noexcept;
    [[nodiscard]] const TypeSchemaDefinition* Resolve(
        const std::string& canonical_id,
        std::uint32_t version) const noexcept;
    [[nodiscard]] std::optional<std::vector<TypeSchemaDefinition>>
        ResolveClosure(
            const std::vector<SchemaIdentity>& roots,
            RegistryError* error = nullptr) const;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return definitions_.size();
    }

    [[nodiscard]] static RegistryResult ValidateShape(
        const TypeSchemaDefinition& definition);

private:
    using Key = std::pair<std::string, std::uint32_t>;
    using DefinitionMap = std::map<Key, TypeSchemaDefinition>;

    [[nodiscard]] static RegistryResult ValidateCandidate(
        const DefinitionMap& definitions);

    DefinitionMap definitions_;
};

} // namespace savor::runtime::program
