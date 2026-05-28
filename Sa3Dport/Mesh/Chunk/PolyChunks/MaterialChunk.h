#pragma once

#include "Mesh/Chunk/PolyChunk.h"
#include "Structs/Color.h"
#include "Structs/ColorIOType.h"
#include "Structs/EndianIOExtensions.h"

#include <cstdint>
#include <optional>

namespace Sa3Dport::Mesh::Chunk::PolyChunks {

class MaterialChunk : public SizedPolyChunk {
public:
    std::optional<Sa3Dport::Structs::Color> diffuse;
    std::optional<Sa3Dport::Structs::Color> ambient;
    std::optional<Sa3Dport::Structs::Color> specular;
    std::uint8_t specular_exponent = 0;

    MaterialChunk() : SizedPolyChunk(PolyChunkType::Material_Diffuse) {
        diffuse = Sa3Dport::Structs::Color::white();
    }

    [[nodiscard]] bool second() const {
        return (static_cast<std::uint8_t>(type) & 0x08u) != 0;
    }

    [[nodiscard]] std::uint16_t size() const override {
        const auto value = static_cast<std::uint8_t>(type);
        return static_cast<std::uint16_t>(
            2u * ((value & 1u) + ((value >> 1u) & 1u) + ((value >> 2u) & 1u)));
    }

    [[nodiscard]] std::uint8_t source_alpha() const {
        return static_cast<std::uint8_t>((attributes >> 3u) & 7u);
    }

    [[nodiscard]] std::uint8_t destination_alpha() const {
        return static_cast<std::uint8_t>(attributes & 7u);
    }

    [[nodiscard]] static MaterialChunk read(const Sa3Dport::Structs::EndianStackReader& reader,
                                            std::uint32_t& address) {
        const std::uint16_t header = reader.read_u16(address);
        const auto readType = static_cast<PolyChunkType>(header & 0xFFu);
        address += 4u;

        MaterialChunk result;
        result.attributes = static_cast<std::uint8_t>(header >> 8u);

        const auto typeValue = static_cast<std::uint8_t>(readType);
        if ((typeValue & 0x01u) != 0) {
            result.diffuse = Sa3Dport::Structs::EndianIOExtensions::read_color(
                reader, address, Sa3Dport::Structs::ColorIOType::ARGB8_16);
        }

        if ((typeValue & 0x02u) != 0) {
            result.ambient = Sa3Dport::Structs::EndianIOExtensions::read_color(
                reader, address, Sa3Dport::Structs::ColorIOType::ARGB8_16);
        }

        if ((typeValue & 0x04u) != 0) {
            auto spec = Sa3Dport::Structs::EndianIOExtensions::read_color(
                reader, address, Sa3Dport::Structs::ColorIOType::ARGB8_16);
            result.specular_exponent = spec.alpha;
            spec.alpha = 0xFFu;
            result.specular = spec;
        }

        // SA3D.Modeling constructs material chunks with a default diffuse color.
        // Reading ambient/specular-only chunks therefore leaves the diffuse type bit set.
        result.type = static_cast<PolyChunkType>(
            static_cast<std::uint8_t>(PolyChunkType::Material_Diffuse) | (typeValue & 0x0Eu));

        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk::PolyChunks
