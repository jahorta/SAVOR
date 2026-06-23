#include "SoaQSortModel.h"

#include <algorithm>
#include <stdexcept>

namespace savor::predict {
namespace {

constexpr std::size_t kIndexKeyRecordSize = 8;
constexpr std::size_t kBattleTurnOrderActionQueueRecordSize = 12;

void write_u32_be(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xffu);
}

std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (bytes.size() < offset + 4) {
        throw std::invalid_argument("record is too small for u32 read");
    }
    return (static_cast<std::uint32_t>(bytes[offset + 0]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

int signed_subtract_u32(std::uint32_t lhs, std::uint32_t rhs) {
    return static_cast<int>(lhs - rhs);
}

std::span<const std::uint8_t> record_at(
    const std::vector<std::uint8_t>& bytes,
    std::size_t element_size,
    std::size_t index) {
    return std::span<const std::uint8_t>(
        bytes.data() + index * element_size,
        element_size);
}

void swap_record_bytes(
    std::vector<std::uint8_t>& bytes,
    std::size_t element_size,
    std::size_t lhs,
    std::size_t rhs) {
    const auto lhs_offset = lhs * element_size;
    const auto rhs_offset = rhs * element_size;
    for (std::size_t i = 0; i < element_size; ++i) {
        std::swap(bytes[lhs_offset + i], bytes[rhs_offset + i]);
    }
}

int compare_records(
    const std::vector<std::uint8_t>& bytes,
    std::size_t element_size,
    std::size_t lhs,
    std::size_t rhs,
    const SoaQSortComparator& comparator) {
    return comparator(
        record_at(bytes, element_size, lhs),
        record_at(bytes, element_size, rhs));
}

int index_key_comparator(std::span<const std::uint8_t> lhs, std::span<const std::uint8_t> rhs) {
    return signed_subtract_u32(read_u32_be(lhs, 4), read_u32_be(rhs, 4));
}

} // namespace

std::vector<std::uint8_t> simulate_soa_qsort_records(
    std::span<const std::uint8_t> records,
    std::size_t element_size,
    std::size_t count,
    const SoaQSortComparator& comparator) {
    if (element_size == 0) {
        throw std::invalid_argument("element_size must be nonzero");
    }
    if (!comparator) {
        throw std::invalid_argument("comparator is required");
    }
    if (records.size() % element_size != 0) {
        throw std::invalid_argument("record byte count must be a multiple of element_size");
    }
    const auto available_count = records.size() / element_size;
    if (count > available_count) {
        throw std::invalid_argument("qsort count exceeds record count");
    }

    std::vector<std::uint8_t> bytes(records.begin(), records.end());
    if (count < 2) {
        return bytes;
    }

    std::size_t uVar8 = (count >> 1) + 1;
    std::size_t pMidpoint = count >> 1;
    std::size_t pEnd = count - 1;

    while (true) {
        if (uVar8 < 2) {
            swap_record_bytes(bytes, element_size, pMidpoint, pEnd);
            --count;
            if (count == 1) {
                return bytes;
            }
            --pEnd;
        } else {
            --pMidpoint;
            --uVar8;
        }

        std::size_t pvVar6 = uVar8 - 1;
        std::size_t uVar7 = uVar8;
        while ((uVar7 << 1) <= count) {
            uVar7 *= 2;
            std::size_t pvVar5 = uVar7 - 1;

            if (uVar7 < count) {
                const int compare_siblings =
                    compare_records(bytes, element_size, pvVar5, pvVar5 + 1, comparator);
                if (compare_siblings < 0) {
                    ++uVar7;
                    ++pvVar5;
                }
            }

            const int compare_parent =
                compare_records(bytes, element_size, pvVar6, pvVar5, comparator);
            if (compare_parent >= 0) {
                break;
            }

            swap_record_bytes(bytes, element_size, pvVar6, pvVar5);
            pvVar6 = pvVar5;
        }
    }
}

int battle_turn_order_action_queue_comparator(
    std::span<const std::uint8_t> lhs,
    std::span<const std::uint8_t> rhs) {
    return signed_subtract_u32(
        battle_turn_order_action_queue_priority(lhs),
        battle_turn_order_action_queue_priority(rhs));
}

std::vector<std::uint8_t> make_battle_turn_order_action_queue_record(
    std::uint32_t word0,
    std::uint32_t assigned_priority,
    std::uint32_t word8) {
    std::vector<std::uint8_t> bytes(kBattleTurnOrderActionQueueRecordSize);
    write_u32_be(bytes, 0, word0);
    write_u32_be(bytes, 4, assigned_priority);
    write_u32_be(bytes, 8, word8);
    return bytes;
}

std::uint8_t battle_turn_order_action_queue_slot(std::span<const std::uint8_t> record) {
    if (record.empty()) {
        throw std::invalid_argument("action queue record is empty");
    }
    return record[0];
}

std::uint32_t battle_turn_order_action_queue_priority(std::span<const std::uint8_t> record) {
    return read_u32_be(record, 4);
}

std::vector<std::uint8_t> simulate_battle_turn_order_action_queue_qsort(
    std::span<const std::uint8_t> records,
    std::size_t count) {
    return simulate_soa_qsort_records(
        records,
        kBattleTurnOrderActionQueueRecordSize,
        count,
        battle_turn_order_action_queue_comparator);
}

std::vector<int> soa_qsort_indices_by_key_ascending(const std::vector<int>& sort_keys) {
    std::vector<std::uint8_t> records;
    records.reserve(sort_keys.size() * kIndexKeyRecordSize);
    for (std::size_t i = 0; i < sort_keys.size(); ++i) {
        std::vector<std::uint8_t> record(kIndexKeyRecordSize);
        write_u32_be(record, 0, static_cast<std::uint32_t>(i));
        write_u32_be(record, 4, static_cast<std::uint32_t>(sort_keys[i]));
        records.insert(records.end(), record.begin(), record.end());
    }

    const auto sorted = simulate_soa_qsort_records(
        records,
        kIndexKeyRecordSize,
        sort_keys.size(),
        index_key_comparator);

    std::vector<int> indices;
    indices.reserve(sort_keys.size());
    for (std::size_t i = 0; i < sort_keys.size(); ++i) {
        indices.push_back(static_cast<int>(
            read_u32_be(std::span<const std::uint8_t>(
                sorted.data() + i * kIndexKeyRecordSize,
                kIndexKeyRecordSize),
                0)));
    }
    return indices;
}

} // namespace savor::predict
