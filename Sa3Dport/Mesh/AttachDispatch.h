#pragma once

#include "Mesh/Attach.h"
#include "Mesh/Chunk/ChunkAttach.h"

namespace Sa3Dport::Mesh {

inline AttachPtr Attach::read(const Sa3Dport::Structs::EndianStackReader& reader,
                              std::uint32_t address,
                              Sa3Dport::ObjectData::Enums::ModelFormat format,
                              AttachReadContext& context) {
    switch (format) {
    case Sa3Dport::ObjectData::Enums::ModelFormat::SA2:
        return Chunk::ChunkAttach::read(reader, address, context);
    case Sa3Dport::ObjectData::Enums::ModelFormat::SA1:
    case Sa3Dport::ObjectData::Enums::ModelFormat::SADX:
    case Sa3Dport::ObjectData::Enums::ModelFormat::SA2B:
    case Sa3Dport::ObjectData::Enums::ModelFormat::Buffer:
    default:
        throw std::invalid_argument("attach format not implemented for this slice");
    }
}

} // namespace Sa3Dport::Mesh
