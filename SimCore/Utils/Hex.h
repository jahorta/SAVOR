
static inline std::string bytes_to_hex(const void* data, size_t size) {
    static const char* HEX = "0123456789abcdef";
    const auto* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out[2 * i + 0] = HEX[(p[i] >> 4) & 0xF];
        out[2 * i + 1] = HEX[p[i] & 0xF];
    }
    return out;
}

static inline std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::string h; h.reserve(hex.size());
    for (char c : hex) if (!std::isspace(static_cast<unsigned char>(c))) h.push_back(c);
    if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) h = h.substr(2);
    if (h.size() % 2) h.insert(h.begin(), '0');
    std::vector<uint8_t> out; out.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        unsigned int byte = 0;
        std::stringstream ss; ss << std::hex << h.substr(i, 2);
        ss >> byte;
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}