#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace savor::utils {

inline std::string Base64Encode(std::string_view input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const auto b0 = static_cast<unsigned char>(input[i++]);
        const auto b1 = static_cast<unsigned char>(input[i++]);
        const auto b2 = static_cast<unsigned char>(input[i++]);
        out.push_back(kAlphabet[b0 >> 2]);
        out.push_back(kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)]);
        out.push_back(kAlphabet[b2 & 0x3F]);
    }

    const auto remaining = input.size() - i;
    if (remaining == 1) {
        const auto b0 = static_cast<unsigned char>(input[i]);
        out.push_back(kAlphabet[b0 >> 2]);
        out.push_back(kAlphabet[(b0 & 0x03) << 4]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const auto b0 = static_cast<unsigned char>(input[i]);
        const auto b1 = static_cast<unsigned char>(input[i + 1]);
        out.push_back(kAlphabet[b0 >> 2]);
        out.push_back(kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(kAlphabet[(b1 & 0x0F) << 2]);
        out.push_back('=');
    }

    return out;
}

inline std::optional<std::string> Base64Decode(std::string_view input) {
    std::array<int, 256> decode{};
    decode.fill(-1);
    for (int i = 0; i < 26; ++i) {
        decode[static_cast<unsigned char>('A' + i)] = i;
        decode[static_cast<unsigned char>('a' + i)] = 26 + i;
    }
    for (int i = 0; i < 10; ++i) {
        decode[static_cast<unsigned char>('0' + i)] = 52 + i;
    }
    decode[static_cast<unsigned char>('+')] = 62;
    decode[static_cast<unsigned char>('/')] = 63;

    std::string cleaned;
    cleaned.reserve(input.size());
    for (char c : input) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        cleaned.push_back(c);
    }
    if (cleaned.empty()) {
        return std::string{};
    }
    if ((cleaned.size() % 4) != 0) {
        return std::nullopt;
    }

    std::string out;
    out.reserve((cleaned.size() / 4) * 3);

    for (std::size_t i = 0; i < cleaned.size(); i += 4) {
        const char c0 = cleaned[i];
        const char c1 = cleaned[i + 1];
        const char c2 = cleaned[i + 2];
        const char c3 = cleaned[i + 3];

        const int v0 = decode[static_cast<unsigned char>(c0)];
        const int v1 = decode[static_cast<unsigned char>(c1)];
        if (v0 < 0 || v1 < 0) {
            return std::nullopt;
        }

        const bool pad2 = c2 == '=';
        const bool pad3 = c3 == '=';
        if (pad2 && !pad3) {
            return std::nullopt;
        }
        if ((pad2 || pad3) && i + 4 != cleaned.size()) {
            return std::nullopt;
        }

        const int v2 = pad2 ? 0 : decode[static_cast<unsigned char>(c2)];
        const int v3 = pad3 ? 0 : decode[static_cast<unsigned char>(c3)];
        if ((!pad2 && v2 < 0) || (!pad3 && v3 < 0)) {
            return std::nullopt;
        }

        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        if (!pad2) {
            out.push_back(static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
        }
        if (!pad3) {
            out.push_back(static_cast<char>(((v2 & 0x03) << 6) | v3));
        }
    }

    return out;
}

} // namespace savor::utils
