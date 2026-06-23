#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace savor::predict {

using SoaQSortComparator = std::function<int(
    std::span<const std::uint8_t>,
    std::span<const std::uint8_t>)>;

std::vector<std::uint8_t> simulate_soa_qsort_records(
    std::span<const std::uint8_t> records,
    std::size_t element_size,
    std::size_t count,
    const SoaQSortComparator& comparator);

int battle_turn_order_action_queue_comparator(
    std::span<const std::uint8_t> lhs,
    std::span<const std::uint8_t> rhs);

std::vector<std::uint8_t> make_battle_turn_order_action_queue_record(
    std::uint32_t word0,
    std::uint32_t assigned_priority,
    std::uint32_t word8);

std::uint8_t battle_turn_order_action_queue_slot(std::span<const std::uint8_t> record);
std::uint32_t battle_turn_order_action_queue_priority(std::span<const std::uint8_t> record);

std::vector<std::uint8_t> simulate_battle_turn_order_action_queue_qsort(
    std::span<const std::uint8_t> records,
    std::size_t count);

std::vector<int> soa_qsort_indices_by_key_ascending(const std::vector<int>& sort_keys);

} // namespace savor::predict
