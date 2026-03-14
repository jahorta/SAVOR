#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "mbedtls/sha256.h"

// Canonical: program_kind + program_version + blueprint
namespace hash {

    inline std::string sha256(const void* payload, size_t size) {
        unsigned char out[32];
        if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(payload), size, out, 0) != 0) return "";
        static const char* hex = "0123456789abcdef";
        std::string s; s.resize(64);
        for (int i = 0; i < 32; ++i) { s[i * 2] = hex[out[i] >> 4]; s[i * 2 + 1] = hex[out[i] & 15]; }
        return s;
    }

    static inline std::string sha256_of_file(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) throw std::runtime_error("open failed: " + path);
        const std::streamsize size = f.tellg();
        if (size < 0) throw std::runtime_error("tellg failed: " + path);
        std::string buf;
        buf.resize(static_cast<size_t>(size));
        f.seekg(0, std::ios::beg);
        if (!f.read(buf.data(), size)) throw std::runtime_error("read failed: " + path);
        return hash::sha256(buf.data(), buf.size());
    }

}
