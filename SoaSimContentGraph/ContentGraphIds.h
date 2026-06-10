#pragma once

#include <cstdint>
#include <string>

namespace soasim::contentgraph {

[[nodiscard]] std::string normalizeContentPath(std::string path);
[[nodiscard]] std::string filenameLabel(std::string path);
[[nodiscard]] std::string scriptFileNodeId(std::string path);
[[nodiscard]] std::string scriptSectionNodeId(std::string path, std::string sectionName);
[[nodiscard]] std::string basicBlockNodeId(std::string path, std::string sectionName, std::uint32_t offset);
[[nodiscard]] std::string instructionNodeId(std::string path, std::string sectionName, std::uint32_t offset);
[[nodiscard]] std::string mldFileNodeId(std::string path);
[[nodiscard]] std::string mldEntryNodeId(std::string path, std::size_t tableIndex, std::uint32_t entryId, std::uint32_t tableId);
[[nodiscard]] std::string resourceRefNodeId(std::string kind, std::string value);
[[nodiscard]] std::string flagRefNodeId(std::uint32_t flagId);
[[nodiscard]] std::string unknownTargetNodeId(std::string sourcePath, std::string sectionName, std::uint32_t offset, std::uint16_t opcode);
[[nodiscard]] std::string mSectionNameForTableId(std::uint32_t tableId);
[[nodiscard]] std::string scriptPairingKey(std::string path);
[[nodiscard]] std::string mldPairingKey(std::string path);

} // namespace soasim::contentgraph
