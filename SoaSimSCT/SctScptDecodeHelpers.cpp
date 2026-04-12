#include "SctScptDecodeHelpers.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace soasim::sct::detail {

std::string toHexWord(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string stackValue(const std::array<std::string, 20>& stack, std::int32_t index) {
    if (index < 0 || index >= static_cast<std::int32_t>(stack.size()) || stack[index].empty()) {
        return std::string{"?"};
    }
    return stack[index];
}

const char* compareSymbol(std::uint32_t value) {
    switch (value) {
    case 0x00000000:
        return "<";
    case 0x00000001:
        return "<=";
    case 0x00000002:
        return ">";
    case 0x00000003:
        return ">=";
    case 0x00000004:
    case 0x00000005:
        return "==";
    case 0x00000010:
    case 0x00000006:
        return "&";
    case 0x00000011:
    case 0x00000007:
        return "|";
    case 0x00000008:
        return "&&";
    case 0x00000009:
        return "||";
    case 0x0000000a:
        return "=";
    default:
        return "?";
    }
}

const char* arithmeticSymbol(std::uint32_t value) {
    switch (value) {
    case 0x00000012:
    case 0x0000000b:
        return "*";
    case 0x00000013:
    case 0x0000000c:
        return "/";
    case 0x00000014:
    case 0x0000000d:
        return "%";
    case 0x00000015:
    case 0x0000000e:
        return "+";
    case 0x00000016:
    case 0x0000000f:
        return "-";
    default:
        return "?";
    }
}

const char* inputPrefix(std::uint32_t action) {
    switch (action) {
    case 0x50000000:
        return "IntVar: ";
    case 0x40000000:
        return "FloatVar: ";
    case 0x20000000:
        return "BitVar: ";
    case 0x10000000:
        return "ByteVar: ";
    case 0x08000000:
        return "decimal: ";
    case 0x04000000:
        return "float: ";
    default:
        return "";
    }
}

std::string secondaryLabel(std::uint32_t value) {
    switch (value) {
    case 0x00000000:
        return "Gold";
    case 0x00000001:
        return "Reputation";
    case 0x00000002:
        return "Vyse.curHP";
    case 0x00000003:
        return "Aika.curHP";
    case 0x00000004:
        return "Fina.curHP";
    case 0x00000005:
        return "Drachma.curHP";
    case 0x00000006:
        return "Enrique.curHP";
    case 0x00000007:
        return "Gilder.curHP";
    case 0x0000004a:
        return "Vyse.lvl";
    default:
        return {};
    }
}

float floatFromWordBits(std::uint32_t value) {
    float floatValue = 0.0f;
    std::memcpy(&floatValue, &value, sizeof(float));
    return floatValue;
}

} // namespace soasim::sct::detail
