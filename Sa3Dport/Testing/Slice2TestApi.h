#pragma once

#include "File/FileHeaders.h"
#include "File/MetaData.h"
#include "File/NJBlockUtility.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Sa3Dport::Testing::Slice2 {

using NJBlockInfo = File::NJBlockInfo;
using NJBlockRole = File::NJBlockRole;
using NJBlockScanResult = File::NJBlockScanResult;
using MetaData = File::MetaData;
using MetaDataSummary = File::MetaDataSummary;

inline NJBlockScanResult ScanNjBlocks(std::span<const std::byte> data, std::uint32_t address = 0) {
    return File::NJBlockUtility::ScanBlocks(data, address);
}

inline std::vector<NJBlockInfo> NjBlocks(std::span<const std::byte> data, std::uint32_t address = 0) {
    return File::NJBlockUtility::GetBlockAddresses(data, address);
}

inline std::optional<std::uint32_t> FindFirstBlock(std::span<const NJBlockInfo> blocks,
                                                   std::span<const std::uint32_t> headers) {
    return File::NJBlockUtility::FindBlockAddress(blocks, headers);
}

inline NJBlockRole ClassifyHeader(std::uint32_t header) {
    return File::NJBlockUtility::GetRole(header);
}

inline std::string_view RoleName(NJBlockRole role) {
    return File::NJBlockUtility::RoleName(role);
}

inline MetaData ReadMetadataShell(std::span<const std::byte> data,
                                  std::uint32_t address,
                                  int version,
                                  bool hasAnimMorphFiles,
                                  Structs::Endian endian = Structs::Endian::Little) {
    return File::MetaDataReader::Read(data, address, version, hasAnimMorphFiles, endian);
}

inline MetaDataSummary SummarizeMetadata(const MetaData& metaData) {
    return File::MetaDataReader::Summarize(metaData);
}

inline constexpr auto ModelBlockHeaders = File::FileHeaders::ModelBlockHeaders;
inline constexpr auto TextureListBlockHeaders = File::FileHeaders::TextureListBlockHeaders;
inline constexpr auto AnimationBlockHeaders = File::FileHeaders::AnimationBlockHeaders;

} // namespace Sa3Dport::Testing::Slice2
