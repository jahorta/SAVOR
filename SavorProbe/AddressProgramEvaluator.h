#pragma once

#include "ProbeEvent.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace savor::probe {

template <typename ResolveBase, typename ReadGuest>
std::optional<std::uint32_t> evaluate_address_program_fixed(
    std::span<const std::uint8_t> program,
    std::span<const std::uint32_t, 32> gprs,
    bool base_resolver_available,
    ResolveBase&& resolve_base,
    ReadGuest&& read_guest,
    RawAddressTrace* trace)
{
    if (trace)
        *trace = {};
    std::size_t offset = 0;
    std::uint32_t address = 0;
    const auto fail = [&](AddressProgramFailure failure, RawAddressTraceStep* step)
        -> std::optional<std::uint32_t> {
        if (step)
            step->failure = failure;
        if (trace) {
            trace->success = false;
            trace->failure = failure;
            trace->failure_operation = step ? step->index : trace->count;
            trace->final_address = address;
        }
        return std::nullopt;
    };
    while (offset < program.size()) {
        if (trace && trace->count >= trace->operations.size())
            return fail(AddressProgramFailure::OperationLimit, nullptr);
        RawAddressTraceStep local_step;
        auto* step = trace ? &trace->operations[trace->count] : &local_step;
        step->index = trace ? trace->count : 0;
        step->operation = program[offset++];
        step->address_before = address;
        if (trace)
            ++trace->count;
        const auto take_u16 = [&]() -> std::optional<std::uint16_t> {
            if (program.size() - offset < 2) return std::nullopt;
            const auto value = static_cast<std::uint16_t>(program[offset])
                | static_cast<std::uint16_t>(program[offset + 1] << 8);
            offset += 2;
            return value;
        };
        const auto take_u32 = [&]() -> std::optional<std::uint32_t> {
            if (program.size() - offset < 4) return std::nullopt;
            std::uint32_t value = 0;
            std::memcpy(&value, program.data() + offset, sizeof(value));
            offset += 4;
            return value;
        };
        switch (step->operation) {
        case 0x00:
            step->address_after = address;
            if (offset != program.size())
                return fail(AddressProgramFailure::TrailingBytes, step);
            if (trace) {
                trace->success = true;
                trace->failure = AddressProgramFailure::None;
                trace->final_address = address;
            }
            return address;
        case 0x01: {
            const auto key = take_u16();
            if (!key.has_value()) return fail(AddressProgramFailure::TruncatedOperand, step);
            if (!base_resolver_available)
                return fail(AddressProgramFailure::MissingBaseResolver, step);
            const auto base = resolve_base(*key);
            if (!base.has_value()) return fail(AddressProgramFailure::BaseResolutionFailed, step);
            address = *base;
            break;
        }
        case 0x02: {
            std::uint64_t dereferenced = 0;
            if (!read_guest(address, SampleWidth::U32, dereferenced))
                return fail(AddressProgramFailure::GuestReadFailed, step);
            step->has_dereferenced_value = true;
            step->dereferenced_value = dereferenced;
            address = static_cast<std::uint32_t>(dereferenced);
            break;
        }
        case 0x03: {
            const auto value = take_u32();
            if (!value.has_value()) return fail(AddressProgramFailure::TruncatedOperand, step);
            address = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(address) + static_cast<std::int32_t>(*value));
            break;
        }
        case 0x04: {
            const auto count = take_u16();
            const auto stride = take_u16();
            if (!count.has_value() || !stride.has_value())
                return fail(AddressProgramFailure::TruncatedOperand, step);
            address += static_cast<std::uint32_t>(*count) * *stride;
            break;
        }
        case 0x05: {
            const auto value = take_u32();
            if (!value.has_value()) return fail(AddressProgramFailure::TruncatedOperand, step);
            address += *value;
            break;
        }
        case 0x06:
            if (offset >= program.size())
                return fail(AddressProgramFailure::TruncatedOperand, step);
            if (program[offset] >= gprs.size())
                return fail(AddressProgramFailure::InvalidRegister, step);
            address = gprs[program[offset++]];
            break;
        case 0x07: {
            const auto value = take_u32();
            if (!value.has_value()) return fail(AddressProgramFailure::TruncatedOperand, step);
            address = *value;
            break;
        }
        case 0x08: {
            if (offset >= program.size())
                return fail(AddressProgramFailure::TruncatedOperand, step);
            const auto reg = program[offset++];
            const auto stride = take_u32();
            if (reg >= gprs.size()) return fail(AddressProgramFailure::InvalidRegister, step);
            if (!stride.has_value()) return fail(AddressProgramFailure::TruncatedOperand, step);
            address += gprs[reg] * *stride;
            break;
        }
        default:
            return fail(AddressProgramFailure::UnknownOperation, step);
        }
        step->address_after = address;
    }
    return fail(AddressProgramFailure::MissingTerminator, nullptr);
}

} // namespace savor::probe
