// SimCore/DB/ProgramDB/Fingerprint.h
#pragma once
#include <string>
#include <vector>
#include "mbedtls/sha256.h"

// Canonical: program_kind + program_version + blueprint
namespace hash {

    inline std::string sha256(const void* payload, size_t size) {
        unsigned char out[32];
        if (!mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(payload), size, out, 0)) return "";
        static const char* hex = "0123456789abcdef";
        std::string s; s.resize(64);
        for (int i = 0; i < 32; ++i) { s[i * 2] = hex[out[i] >> 4]; s[i * 2 + 1] = hex[out[i] & 15]; }
        return s;
    }

}
