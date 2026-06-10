#include "ContentGraphIds.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace soasim::contentgraph {
namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string hexOffset(std::uint32_t offset) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << offset;
    return out.str();
}

std::string stemLower(std::string path) {
    return lowerCopy(std::filesystem::path(path).stem().generic_string());
}

} // namespace

std::string normalizeContentPath(std::string path) {
    return lowerCopy(std::filesystem::path(std::move(path)).lexically_normal().generic_string());
}

std::string filenameLabel(std::string path) {
    return std::filesystem::path(std::move(path)).filename().generic_string();
}

std::string scriptFileNodeId(std::string path) {
    return "script_file:" + normalizeContentPath(std::move(path));
}

std::string scriptSectionNodeId(std::string path, std::string sectionName) {
    return scriptFileNodeId(std::move(path)) + ":section:" + sectionName;
}

std::string basicBlockNodeId(std::string path, std::string sectionName, std::uint32_t offset) {
    return scriptSectionNodeId(std::move(path), std::move(sectionName)) + ":block:" + hexOffset(offset);
}

std::string instructionNodeId(std::string path, std::string sectionName, std::uint32_t offset) {
    return scriptSectionNodeId(std::move(path), std::move(sectionName)) + ":instruction:" + hexOffset(offset);
}

std::string mldFileNodeId(std::string path) {
    return "mld_file:" + normalizeContentPath(std::move(path));
}

std::string mldEntryNodeId(std::string path, std::size_t tableIndex, std::uint32_t entryId, std::uint32_t tableId) {
    return mldFileNodeId(std::move(path)) + ":entry:" + std::to_string(tableIndex) + ":" + std::to_string(entryId)
        + ":" + std::to_string(tableId);
}

std::string resourceRefNodeId(std::string kind, std::string value) {
    return "resource:" + lowerCopy(std::move(kind)) + ":" + lowerCopy(std::move(value));
}

std::string flagRefNodeId(std::uint32_t flagId) {
    return "flag:" + std::to_string(flagId);
}

std::string unknownTargetNodeId(std::string sourcePath, std::string sectionName, std::uint32_t offset, std::uint16_t opcode) {
    return "unknown:" + normalizeContentPath(std::move(sourcePath)) + ":" + sectionName + ":" + hexOffset(offset)
        + ":opcode:" + std::to_string(opcode);
}

std::string mSectionNameForTableId(std::uint32_t tableId) {
    std::ostringstream out;
    out << "M" << std::setw(5) << std::setfill('0') << tableId;
    return out.str();
}

std::string scriptPairingKey(std::string path) {
    auto stem = stemLower(std::move(path));
    if (stem.starts_with("me") && stem.size() > 2) {
        return stem.substr(2);
    }
    return stem;
}

std::string mldPairingKey(std::string path) {
    auto stem = stemLower(std::move(path));
    if (stem.starts_with("a") && stem.size() > 1) {
        return stem.substr(1);
    }
    return stem;
}

} // namespace soasim::contentgraph
