#include "ProgramTypes.h"

#include <algorithm>

namespace savor::runtime::program {

bool ContentHash256::empty() const noexcept
{
    return std::ranges::all_of(bytes, [](Byte value) { return value == 0; });
}

std::string ContentHash256::ToHex() const
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string result(64, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        result[index * 2] = kHex[bytes[index] >> 4];
        result[index * 2 + 1] = kHex[bytes[index] & 0x0f];
    }
    return result;
}

std::optional<ContentHash256> ContentHash256::FromHex(const std::string& text)
{
    if (text.size() != 64)
        return std::nullopt;

    auto decode = [](char value) -> std::optional<Byte> {
        if (value >= '0' && value <= '9')
            return static_cast<Byte>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<Byte>(value - 'a' + 10);
        return std::nullopt;
    };

    ContentHash256 result;
    for (std::size_t index = 0; index < result.bytes.size(); ++index)
    {
        const auto high = decode(text[index * 2]);
        const auto low = decode(text[index * 2 + 1]);
        if (!high || !low)
            return std::nullopt;
        result.bytes[index] = static_cast<Byte>((*high << 4) | *low);
    }
    return result;
}

TypeRef TypeRef::Builtin(BuiltinType type) noexcept
{
    return TypeRef{
        .builtin = type,
        .named = std::nullopt,
    };
}

TypeRef TypeRef::Named(SchemaIdentity identity)
{
    return TypeRef{
        .builtin = BuiltinType::Unit,
        .named = std::move(identity),
    };
}

} // namespace savor::runtime::program
