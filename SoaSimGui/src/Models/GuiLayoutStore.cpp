#include "GuiLayoutStore.h"
#include <string>

static std::string b64enc(const unsigned char* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= (data[i + 1] << 8);
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back((i + 1 < len) ? tbl[(v >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? tbl[v & 63] : '=');
    }
    return out;
}
static std::string b64dec(const std::string& s) {
    auto val = [](char c)->int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62; if (c == '/') return 63; return -1;
        };
    std::string out; out.reserve((s.size() / 4) * 3);
    unsigned v = 0; int vb = 0;
    for (char c : s) {
        if (c == '=') break;
        int d = val(c); if (d < 0) continue;
        v = (v << 6) | (unsigned)d; vb += 6;
        if (vb >= 8) { vb -= 8; out.push_back((char)((v >> vb) & 0xFF)); }
    }
    return out;
}

void GuiLayoutStore::LoadLayout(IniDoc& doc, std::string& out_ini_text) {
    auto& kv = doc.ensure_section("Gui/Layout");
    const auto blob_b64 = kv.get("imgui_ini_b64", "");
    if (blob_b64.empty()) { out_ini_text.clear(); return; }
    out_ini_text = b64dec(blob_b64);
}

void GuiLayoutStore::SaveLayout(IniDoc& doc, const std::string& ini_text) {
    auto& kv = doc.ensure_section("Gui/Layout");
    std::string b64 = b64enc(reinterpret_cast<const unsigned char*>(ini_text.data()), ini_text.size());
    kv.set("imgui_ini_b64", b64);
}
