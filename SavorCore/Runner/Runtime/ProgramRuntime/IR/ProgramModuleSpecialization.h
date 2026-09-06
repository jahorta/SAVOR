#pragma once

#include "Utils/Hash.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace savor::runtime::program {

struct ProgramModuleSpecializationFieldV1
{
    std::string name;
    std::string value;
};

inline void AppendProgramModuleSpecializationValueV1(
    std::string& encoded,
    std::string_view value)
{
    encoded += std::to_string(value.size());
    encoded.push_back(':');
    encoded.append(value);
}

[[nodiscard]] inline std::string ComputeProgramModuleSpecializationHashV1(
    std::string_view base_canonical_id,
    std::uint32_t generator_contract_version,
    std::span<const ProgramModuleSpecializationFieldV1> fields)
{
    if (base_canonical_id.empty() || generator_contract_version == 0)
        throw std::invalid_argument(
            "Program module specialization requires a base ID and version");

    std::string encoded = "SAVOR-PROGRAM-MODULE-SPECIALIZATION-V1";
    AppendProgramModuleSpecializationValueV1(encoded, base_canonical_id);
    AppendProgramModuleSpecializationValueV1(
        encoded, std::to_string(generator_contract_version));
    AppendProgramModuleSpecializationValueV1(
        encoded, std::to_string(fields.size()));
    for (const auto& field : fields)
    {
        if (field.name.empty())
            throw std::invalid_argument(
                "Program module specialization field name is empty");
        AppendProgramModuleSpecializationValueV1(encoded, field.name);
        AppendProgramModuleSpecializationValueV1(encoded, field.value);
    }
    return hash::sha256(encoded.data(), encoded.size());
}

[[nodiscard]] inline std::string MakeSpecializedProgramModuleIdV1(
    std::string_view base_canonical_id,
    std::uint32_t generator_contract_version,
    std::span<const ProgramModuleSpecializationFieldV1> fields)
{
    return std::string(base_canonical_id) + ".specialization." +
        ComputeProgramModuleSpecializationHashV1(
            base_canonical_id, generator_contract_version, fields);
}

} // namespace savor::runtime::program
