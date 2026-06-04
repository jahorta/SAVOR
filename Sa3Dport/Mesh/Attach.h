#pragma once

#include "Mesh/Enums.h"
#include "ObjectData/Enums/ModelFormat.h"
#include "Structs/Bounds.h"
#include "Structs/EndianStackReader.h"
#include "Structs/PointerIO.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace Sa3Dport::Mesh {

class Attach;
using AttachPtr = std::shared_ptr<Attach>;

struct AttachReadContext {
    std::uint32_t image_base = 0;
    std::unordered_map<std::uint32_t, AttachPtr> attaches;
};

class Attach {
public:
    virtual ~Attach() = default;

    std::string label = "attach_";
    Sa3Dport::Structs::Bounds mesh_bounds {};
    std::uint32_t source_address = 0;

    [[nodiscard]] virtual AttachFormat format() const {
        return AttachFormat::Buffer;
    }

    [[nodiscard]] virtual bool check_has_weights() const {
        return false;
    }

    [[nodiscard]] static AttachPtr read(const Sa3Dport::Structs::EndianStackReader& reader,
                                        std::uint32_t address,
                                        Sa3Dport::ObjectData::Enums::ModelFormat format,
                                        AttachReadContext& context);

protected:
    [[nodiscard]] static std::optional<std::uint32_t> read_pointer(
        const Sa3Dport::Structs::EndianStackReader& reader,
        std::uint32_t address,
        std::uint32_t imageBase) {
        return Sa3Dport::Structs::PointerIO::read_nullable_pointer_subtract_base(reader, address, imageBase);
    }
};

} // namespace Sa3Dport::Mesh
