#pragma once

#include "../../RuntimeTypes.h"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>

namespace savor::runtime::program {

using Byte = std::uint8_t;

struct ContentHash256
{
    std::array<Byte, 32> bytes{};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string ToHex() const;
    [[nodiscard]] static std::optional<ContentHash256> FromHex(const std::string& text);

    auto operator<=>(const ContentHash256&) const = default;
};

struct ModuleIdentity
{
    std::string canonical_id;
    std::uint32_t revision = 0;
    ContentHash256 module_hash;

    auto operator<=>(const ModuleIdentity&) const = default;
};

struct SchemaIdentity
{
    std::string canonical_id;
    std::uint32_t version = 0;
    ContentHash256 schema_hash;

    auto operator<=>(const SchemaIdentity&) const = default;
};

struct ExactDependencyIdentity
{
    std::string canonical_id;
    std::uint32_t version = 0;
    ContentHash256 signature_hash;

    auto operator<=>(const ExactDependencyIdentity&) const = default;
};

struct ModuleImportIdentity
{
    ModuleIdentity module;
    std::uint32_t ir_version = 0;

    auto operator<=>(const ModuleImportIdentity&) const = default;
};

struct CapabilityPackIdentity
{
    std::string canonical_id;
    std::uint32_t version = 0;
    ContentHash256 manifest_hash;

    auto operator<=>(const CapabilityPackIdentity&) const = default;
};

struct ProgramFunctionIdTag;
struct ProgramBlockIdTag;
struct ProgramValueIdTag;
struct ProgramInstructionIdTag;
struct ProgramSourceLocationIdTag;
struct ProgramScopeIdTag;
struct ProgramResourceHandleIdTag;
struct ProgramTraceSequenceTag;
struct ProgramEmissionSequenceTag;
struct ProgramArtifactSequenceTag;

using ProgramFunctionId = StrongId<ProgramFunctionIdTag>;
using ProgramBlockId = StrongId<ProgramBlockIdTag>;
using ProgramValueId = StrongId<ProgramValueIdTag>;
using ProgramInstructionId = StrongId<ProgramInstructionIdTag>;
using ProgramSourceLocationId = StrongId<ProgramSourceLocationIdTag>;
using ProgramScopeId = StrongId<ProgramScopeIdTag>;
using ProgramResourceHandleId = StrongId<ProgramResourceHandleIdTag>;
using ProgramTraceSequence = StrongId<ProgramTraceSequenceTag>;
using ProgramEmissionSequence = StrongId<ProgramEmissionSequenceTag>;
using ProgramArtifactSequence = StrongId<ProgramArtifactSequenceTag>;

static_assert(!std::is_convertible_v<ProgramFunctionId, ProgramBlockId>);
static_assert(!std::is_convertible_v<ProgramValueId, ProgramInstructionId>);
static_assert(!std::is_convertible_v<ProgramScopeId, ProgramResourceHandleId>);
static_assert(!std::is_convertible_v<ProgramTraceSequence, ProgramEmissionSequence>);

} // namespace savor::runtime::program
