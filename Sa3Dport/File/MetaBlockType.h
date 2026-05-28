#pragma once

#include <cstdint>

namespace Sa3Dport::File {

enum class MetaBlockType : std::uint32_t {
    Label = 0x4C42414Cu,
    Animation = 0x4D494E41u,
    Morph = 0x46524F4Du,
    Author = 0x48545541u,
    Tool = 0x4C4F4F54u,
    Description = 0x43534544u,
    Texture = 0x00584554u,
    ActionName = 0x4143544Eu,
    ObjectName = 0x4F424A4Eu,
    Weight = 0x54484757u,
    End = 0x00444E45u,
};

} // namespace Sa3Dport::File
