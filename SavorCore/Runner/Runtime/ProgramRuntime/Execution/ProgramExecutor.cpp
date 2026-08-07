#include "ProgramExecutor.h"
#include "../Model/ProgramValueArena.h"
#include "../Registry/CanonicalActionCatalog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace savor::runtime::program {
namespace {

template <class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

constexpr ProgramScopeId kInvocationScope{
    std::numeric_limits<std::uint64_t>::max()};

[[nodiscard]] const ProgramValue* FindValue(
    const ProgramValueGraph& graph,
    ProgramValueId id) noexcept
{
    const auto found = std::find_if(
        graph.values.begin(),
        graph.values.end(),
        [id](const ProgramValue& value) { return value.id == id; });
    return found == graph.values.end() ? nullptr : &*found;
}

[[nodiscard]] const ProgramValue* RootValue(
    const ProgramValueGraph& graph) noexcept
{
    return FindValue(graph, graph.root);
}

void RemapPayload(
    ProgramValuePayload& payload,
    const std::map<std::uint64_t, std::uint64_t>& remap)
{
    const auto replace = [&remap](ProgramValueId id) {
        const auto found = remap.find(id.value());
        return found == remap.end()
            ? ProgramValueId{}
            : ProgramValueId(found->second);
    };
    std::visit(
        Overloaded{
            [&replace](OptionalValue& value) {
                if (value.value)
                    value.value = replace(*value.value);
            },
            [&replace](RecordValue& value) {
                for (ProgramValueId& field : value.fields)
                    field = replace(field);
            },
            [&replace](ListValue& value) {
                for (ProgramValueId& element : value.elements)
                    element = replace(element);
            },
            [](auto&) {}},
        payload);
}

[[nodiscard]] ProgramValueId AppendGraph(
    ProgramValueGraph& destination,
    const ProgramValueGraph& source,
    std::uint64_t& next_id)
{
    std::map<std::uint64_t, std::uint64_t> remap;
    for (const ProgramValue& value : source.values)
        remap.emplace(value.id.value(), next_id++);
    for (const ProgramValue& value : source.values)
    {
        ProgramValue copy = value;
        copy.id = ProgramValueId(remap[value.id.value()]);
        RemapPayload(copy.payload, remap);
        destination.values.push_back(std::move(copy));
    }
    const auto root = remap.find(source.root.value());
    return root == remap.end() ? ProgramValueId{}
                               : ProgramValueId(root->second);
}

[[nodiscard]] ProgramValueGraph ScalarGraph(
    TypeRef type,
    ProgramValuePayload payload)
{
    return {
        ProgramValueId(1),
        {ProgramValue{
            ProgramValueId(1),
            std::move(type),
            std::move(payload)}}};
}

[[nodiscard]] ProgramValueGraph UnitGraph()
{
    return ScalarGraph(
        TypeRef::Builtin(BuiltinType::Unit),
        UnitValue{});
}

[[nodiscard]] ProgramValueGraph CompositeGraph(
    TypeRef type,
    std::span<const ProgramValueGraph> children,
    bool list)
{
    ProgramValueGraph result;
    std::uint64_t next_id = 1;
    std::vector<ProgramValueId> roots;
    roots.reserve(children.size());
    for (const ProgramValueGraph& child : children)
        roots.push_back(AppendGraph(result, child, next_id));
    result.root = ProgramValueId(next_id++);
    result.values.push_back(ProgramValue{
        result.root,
        std::move(type),
        list
            ? ProgramValuePayload(ListValue{std::move(roots)})
            : ProgramValuePayload(RecordValue{std::move(roots)})});
    return result;
}

[[nodiscard]] ProgramValueGraph OptionalGraph(
    TypeRef type,
    const ProgramValueGraph* child)
{
    ProgramValueGraph result;
    std::uint64_t next_id = 1;
    std::optional<ProgramValueId> root;
    if (child)
        root = AppendGraph(result, *child, next_id);
    result.root = ProgramValueId(next_id++);
    result.values.push_back(ProgramValue{
        result.root,
        std::move(type),
        OptionalValue{root}});
    return result;
}

[[nodiscard]] ProgramValueGraph ExtractGraph(
    const ProgramValueGraph& source,
    ProgramValueId root)
{
    ProgramValueGraph result;
    if (!FindValue(source, root))
        return result;

    std::vector<ProgramValueId> pending{root};
    std::set<std::uint64_t> reachable;
    while (!pending.empty())
    {
        const ProgramValueId current = pending.back();
        pending.pop_back();
        if (!reachable.insert(current.value()).second)
            continue;
        const ProgramValue* value = FindValue(source, current);
        if (!value)
            return {};
        std::visit(
            Overloaded{
                [&pending](const OptionalValue& optional) {
                    if (optional.value)
                        pending.push_back(*optional.value);
                },
                [&pending](const RecordValue& record) {
                    pending.insert(
                        pending.end(),
                        record.fields.begin(),
                        record.fields.end());
                },
                [&pending](const ListValue& list) {
                    pending.insert(
                        pending.end(),
                        list.elements.begin(),
                        list.elements.end());
                },
                [](const auto&) {}},
            value->payload);
    }

    ProgramValueGraph selected;
    selected.root = root;
    for (const ProgramValue& value : source.values)
    {
        if (reachable.contains(value.id.value()))
            selected.values.push_back(value);
    }
    std::uint64_t next_id = 1;
    result.root = AppendGraph(result, selected, next_id);
    return result;
}

[[nodiscard]] ProgramValueGraph FromLiteral(
    const LiteralValue& literal)
{
    return std::visit(
        [&literal](const auto& value) {
            return ScalarGraph(literal.type, ProgramValuePayload(value));
        },
        literal.payload);
}

struct Number
{
    bool valid = false;
    bool floating = false;
    bool signed_integer = false;
    std::uint64_t unsigned_value = 0;
    std::int64_t signed_value = 0;
    long double floating_value = 0;
};

[[nodiscard]] Number ToNumber(const ProgramValueGraph& graph)
{
    const ProgramValue* root = RootValue(graph);
    if (!root)
        return {};
    return std::visit(
        Overloaded{
            [](std::uint8_t value) {
                return Number{true, false, false, value, 0,
                              static_cast<long double>(value)};
            },
            [](std::uint16_t value) {
                return Number{true, false, false, value, 0,
                              static_cast<long double>(value)};
            },
            [](std::uint32_t value) {
                return Number{true, false, false, value, 0,
                              static_cast<long double>(value)};
            },
            [](std::uint64_t value) {
                return Number{true, false, false, value, 0,
                              static_cast<long double>(value)};
            },
            [](std::int32_t value) {
                return Number{true, false, true, 0, value,
                              static_cast<long double>(value)};
            },
            [](std::int64_t value) {
                return Number{true, false, true, 0, value,
                              static_cast<long double>(value)};
            },
            [](float value) {
                return Number{std::isfinite(value), true, true, 0, 0,
                              static_cast<long double>(value)};
            },
            [](double value) {
                return Number{std::isfinite(value), true, true, 0, 0,
                              static_cast<long double>(value)};
            },
            [](const auto&) { return Number{}; }},
        root->payload);
}

[[nodiscard]] std::optional<ProgramValueGraph> NumberGraph(
    TypeRef type,
    long double value)
{
    if (!std::isfinite(value))
        return std::nullopt;
    const auto integral = [&]<typename T>() -> std::optional<ProgramValueGraph> {
        if (std::trunc(value) != value ||
            value < static_cast<long double>(
                std::numeric_limits<T>::lowest()) ||
            value > static_cast<long double>(
                std::numeric_limits<T>::max()))
        {
            return std::nullopt;
        }
        return ScalarGraph(type, static_cast<T>(value));
    };
    switch (type.builtin)
    {
    case BuiltinType::U8:
        return integral.template operator()<std::uint8_t>();
    case BuiltinType::U16:
        return integral.template operator()<std::uint16_t>();
    case BuiltinType::U32:
        return integral.template operator()<std::uint32_t>();
    case BuiltinType::U64:
        return integral.template operator()<std::uint64_t>();
    case BuiltinType::I32:
        return integral.template operator()<std::int32_t>();
    case BuiltinType::I64:
        return integral.template operator()<std::int64_t>();
    case BuiltinType::F32:
        if (value < -std::numeric_limits<float>::max() ||
            value > std::numeric_limits<float>::max())
        {
            return std::nullopt;
        }
        return ScalarGraph(type, static_cast<float>(value));
    case BuiltinType::F64:
        if (value < -std::numeric_limits<double>::max() ||
            value > std::numeric_limits<double>::max())
        {
            return std::nullopt;
        }
        return ScalarGraph(type, static_cast<double>(value));
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool IsIntegerBuiltin(BuiltinType type) noexcept
{
    switch (type)
    {
    case BuiltinType::U8:
    case BuiltinType::U16:
    case BuiltinType::U32:
    case BuiltinType::U64:
    case BuiltinType::I32:
    case BuiltinType::I64:
        return true;
    default:
        return false;
    }
}

template <typename T>
[[nodiscard]] std::optional<T> ExactIntegerConversion(
    const Number& number) noexcept
{
    static_assert(std::is_integral_v<T>);
    if (!number.valid)
        return std::nullopt;
    if (number.floating)
    {
        const long double value = number.floating_value;
        if (!std::isfinite(value) ||
            std::trunc(value) != value)
        {
            return std::nullopt;
        }
        if constexpr (std::is_unsigned_v<T>)
        {
            const long double upper =
                std::ldexp(
                    static_cast<long double>(1),
                    std::numeric_limits<T>::digits);
            if (value < 0 || value >= upper)
                return std::nullopt;
        }
        else
        {
            const long double upper =
                std::ldexp(
                    static_cast<long double>(1),
                    std::numeric_limits<T>::digits);
            if (value < -upper || value >= upper)
                return std::nullopt;
        }
        return static_cast<T>(value);
    }

    if constexpr (std::is_unsigned_v<T>)
    {
        if (number.signed_integer)
        {
            if (number.signed_value < 0)
                return std::nullopt;
            const auto value =
                static_cast<std::uint64_t>(number.signed_value);
            if (value > std::numeric_limits<T>::max())
                return std::nullopt;
            return static_cast<T>(value);
        }
        if (number.unsigned_value >
            std::numeric_limits<T>::max())
        {
            return std::nullopt;
        }
        return static_cast<T>(number.unsigned_value);
    }
    else
    {
        if (number.signed_integer)
        {
            if (number.signed_value <
                    static_cast<std::int64_t>(
                        std::numeric_limits<T>::lowest()) ||
                number.signed_value >
                    static_cast<std::int64_t>(
                        std::numeric_limits<T>::max()))
            {
                return std::nullopt;
            }
            return static_cast<T>(number.signed_value);
        }
        if (number.unsigned_value >
            static_cast<std::uint64_t>(
                std::numeric_limits<T>::max()))
        {
            return std::nullopt;
        }
        return static_cast<T>(number.unsigned_value);
    }
}

template <typename F>
[[nodiscard]] std::optional<ProgramValueGraph>
ExactFloatingConversionGraph(
    TypeRef type,
    const Number& number)
{
    static_assert(std::is_floating_point_v<F>);
    if (!number.valid)
        return std::nullopt;
    if (number.floating)
    {
        const long double value = number.floating_value;
        if (value < -std::numeric_limits<F>::max() ||
            value > std::numeric_limits<F>::max())
        {
            return std::nullopt;
        }
        const F converted = static_cast<F>(value);
        if (!std::isfinite(converted))
            return std::nullopt;
        return ScalarGraph(type, converted);
    }

    const auto exact_magnitude = [](std::uint64_t magnitude) {
        if (magnitude == 0)
            return true;
        const unsigned width = std::bit_width(magnitude);
        constexpr unsigned digits =
            std::numeric_limits<F>::digits;
        if (width <= digits)
            return true;
        const unsigned discarded = width - digits;
        const std::uint64_t mask =
            (std::uint64_t{1} << discarded) - 1;
        return (magnitude & mask) == 0;
    };

    std::uint64_t magnitude = number.unsigned_value;
    if (number.signed_integer)
    {
        const std::uint64_t bits =
            static_cast<std::uint64_t>(number.signed_value);
        magnitude = number.signed_value < 0
            ? std::uint64_t{0} - bits
            : bits;
    }
    if (!exact_magnitude(magnitude))
        return std::nullopt;
    const F converted = number.signed_integer
        ? static_cast<F>(number.signed_value)
        : static_cast<F>(number.unsigned_value);
    if (!std::isfinite(converted))
        return std::nullopt;
    return ScalarGraph(type, converted);
}

[[nodiscard]] std::optional<ProgramValueGraph>
ExactIntegerConversionGraph(
    TypeRef type,
    const Number& number)
{
    switch (type.builtin)
    {
    case BuiltinType::U8:
        if (const auto value =
                ExactIntegerConversion<std::uint8_t>(number))
            return ScalarGraph(type, *value);
        break;
    case BuiltinType::U16:
        if (const auto value =
                ExactIntegerConversion<std::uint16_t>(number))
            return ScalarGraph(type, *value);
        break;
    case BuiltinType::U32:
        if (const auto value =
                ExactIntegerConversion<std::uint32_t>(number))
            return ScalarGraph(type, *value);
        break;
    case BuiltinType::U64:
        if (const auto value =
                ExactIntegerConversion<std::uint64_t>(number))
            return ScalarGraph(type, *value);
        break;
    case BuiltinType::I32:
        if (const auto value =
                ExactIntegerConversion<std::int32_t>(number))
            return ScalarGraph(type, *value);
        break;
    case BuiltinType::I64:
        if (const auto value =
                ExactIntegerConversion<std::int64_t>(number))
            return ScalarGraph(type, *value);
        break;
    default:
        break;
    }
    return std::nullopt;
}

template <typename T>
[[nodiscard]] std::optional<T> CheckedAdd(T lhs, T rhs) noexcept
{
    if constexpr (std::is_unsigned_v<T>)
    {
        if (lhs > std::numeric_limits<T>::max() - rhs)
            return std::nullopt;
    }
    else
    {
        if ((rhs > 0 &&
             lhs > std::numeric_limits<T>::max() - rhs) ||
            (rhs < 0 &&
             lhs < std::numeric_limits<T>::lowest() - rhs))
        {
            return std::nullopt;
        }
    }
    return static_cast<T>(lhs + rhs);
}

template <typename T>
[[nodiscard]] std::optional<T> CheckedSubtract(
    T lhs,
    T rhs) noexcept
{
    if constexpr (std::is_unsigned_v<T>)
    {
        if (lhs < rhs)
            return std::nullopt;
    }
    else
    {
        if ((rhs < 0 &&
             lhs > std::numeric_limits<T>::max() + rhs) ||
            (rhs > 0 &&
             lhs < std::numeric_limits<T>::lowest() + rhs))
        {
            return std::nullopt;
        }
    }
    return static_cast<T>(lhs - rhs);
}

template <typename T>
[[nodiscard]] std::optional<T> CheckedMultiply(
    T lhs,
    T rhs) noexcept
{
    using U = std::make_unsigned_t<T>;
    if constexpr (std::is_unsigned_v<T>)
    {
        if (lhs != 0 &&
            rhs > std::numeric_limits<T>::max() / lhs)
        {
            return std::nullopt;
        }
        return static_cast<T>(lhs * rhs);
    }
    else
    {
        const bool negative = (lhs < 0) != (rhs < 0);
        const auto magnitude = [](T value) noexcept -> U {
            const U bits = static_cast<U>(value);
            return value < 0 ? U(0) - bits : bits;
        };
        const U lhs_magnitude = magnitude(lhs);
        const U rhs_magnitude = magnitude(rhs);
        const U negative_limit =
            static_cast<U>(std::numeric_limits<T>::max()) + U(1);
        const U limit = negative
            ? negative_limit
            : static_cast<U>(std::numeric_limits<T>::max());
        if (lhs_magnitude != 0 &&
            rhs_magnitude > limit / lhs_magnitude)
        {
            return std::nullopt;
        }
        const U product = lhs_magnitude * rhs_magnitude;
        if (!negative)
            return static_cast<T>(product);
        if (product == negative_limit)
            return std::numeric_limits<T>::lowest();
        return static_cast<T>(-static_cast<T>(product));
    }
}

template <typename T>
[[nodiscard]] std::optional<T> CheckedDivide(
    T lhs,
    T rhs) noexcept
{
    if (rhs == 0)
        return std::nullopt;
    if constexpr (std::is_signed_v<T>)
    {
        if (lhs == std::numeric_limits<T>::lowest() &&
            rhs == T(-1))
        {
            return std::nullopt;
        }
    }
    return static_cast<T>(lhs / rhs);
}

template <typename T>
[[nodiscard]] std::optional<T> CheckedRemainder(
    T lhs,
    T rhs) noexcept
{
    if (rhs == 0)
        return std::nullopt;
    if constexpr (std::is_signed_v<T>)
    {
        if (lhs == std::numeric_limits<T>::lowest() &&
            rhs == T(-1))
        {
            // The mathematical remainder is zero, but C++ division for this
            // pair is unrepresentable and `%` has the same undefined case.
            return T(0);
        }
    }
    return static_cast<T>(lhs % rhs);
}

template <typename T>
[[nodiscard]] std::optional<T> CheckedNegate(T value) noexcept
{
    if constexpr (std::is_unsigned_v<T>)
    {
        if (value != 0)
            return std::nullopt;
        return T(0);
    }
    else
    {
        if (value == std::numeric_limits<T>::lowest())
            return std::nullopt;
        return static_cast<T>(-value);
    }
}

template <typename T>
[[nodiscard]] std::optional<ProgramValueGraph>
CheckedIntegerBinaryGraph(
    TypeRef type,
    const ProgramValueGraph& lhs_graph,
    const ProgramValueGraph& rhs_graph,
    InstructionOpcode opcode)
{
    const ProgramValue* lhs_root = RootValue(lhs_graph);
    const ProgramValue* rhs_root = RootValue(rhs_graph);
    const T* lhs = lhs_root
        ? std::get_if<T>(&lhs_root->payload)
        : nullptr;
    const T* rhs = rhs_root
        ? std::get_if<T>(&rhs_root->payload)
        : nullptr;
    if (!lhs || !rhs)
        return std::nullopt;

    std::optional<T> result;
    switch (opcode)
    {
    case InstructionOpcode::AddChecked:
        result = CheckedAdd(*lhs, *rhs);
        break;
    case InstructionOpcode::SubtractChecked:
        result = CheckedSubtract(*lhs, *rhs);
        break;
    case InstructionOpcode::MultiplyChecked:
        result = CheckedMultiply(*lhs, *rhs);
        break;
    case InstructionOpcode::DivideChecked:
        result = CheckedDivide(*lhs, *rhs);
        break;
    case InstructionOpcode::RemainderChecked:
        result = CheckedRemainder(*lhs, *rhs);
        break;
    default:
        return std::nullopt;
    }
    if (!result)
        return std::nullopt;
    return ScalarGraph(type, *result);
}

[[nodiscard]] std::optional<ProgramValueGraph>
CheckedIntegerBinaryGraph(
    TypeRef type,
    const ProgramValueGraph& lhs,
    const ProgramValueGraph& rhs,
    InstructionOpcode opcode)
{
    switch (type.builtin)
    {
    case BuiltinType::U8:
        return CheckedIntegerBinaryGraph<std::uint8_t>(
            type, lhs, rhs, opcode);
    case BuiltinType::U16:
        return CheckedIntegerBinaryGraph<std::uint16_t>(
            type, lhs, rhs, opcode);
    case BuiltinType::U32:
        return CheckedIntegerBinaryGraph<std::uint32_t>(
            type, lhs, rhs, opcode);
    case BuiltinType::U64:
        return CheckedIntegerBinaryGraph<std::uint64_t>(
            type, lhs, rhs, opcode);
    case BuiltinType::I32:
        return CheckedIntegerBinaryGraph<std::int32_t>(
            type, lhs, rhs, opcode);
    case BuiltinType::I64:
        return CheckedIntegerBinaryGraph<std::int64_t>(
            type, lhs, rhs, opcode);
    default:
        return std::nullopt;
    }
}

template <typename T>
[[nodiscard]] std::optional<ProgramValueGraph>
CheckedIntegerNegationGraph(
    TypeRef type,
    const ProgramValueGraph& source)
{
    const ProgramValue* root = RootValue(source);
    const T* value =
        root ? std::get_if<T>(&root->payload) : nullptr;
    if (!value)
        return std::nullopt;
    const std::optional<T> result = CheckedNegate(*value);
    if (!result)
        return std::nullopt;
    return ScalarGraph(type, *result);
}

[[nodiscard]] std::optional<ProgramValueGraph>
CheckedIntegerNegationGraph(
    TypeRef type,
    const ProgramValueGraph& source)
{
    switch (type.builtin)
    {
    case BuiltinType::U8:
        return CheckedIntegerNegationGraph<std::uint8_t>(
            type, source);
    case BuiltinType::U16:
        return CheckedIntegerNegationGraph<std::uint16_t>(
            type, source);
    case BuiltinType::U32:
        return CheckedIntegerNegationGraph<std::uint32_t>(
            type, source);
    case BuiltinType::U64:
        return CheckedIntegerNegationGraph<std::uint64_t>(
            type, source);
    case BuiltinType::I32:
        return CheckedIntegerNegationGraph<std::int32_t>(
            type, source);
    case BuiltinType::I64:
        return CheckedIntegerNegationGraph<std::int64_t>(
            type, source);
    default:
        return std::nullopt;
    }
}

template <typename T>
[[nodiscard]] std::optional<int> CompareExactInteger(
    const ProgramValueGraph& lhs_graph,
    const ProgramValueGraph& rhs_graph)
{
    const ProgramValue* lhs_root = RootValue(lhs_graph);
    const ProgramValue* rhs_root = RootValue(rhs_graph);
    const T* lhs = lhs_root
        ? std::get_if<T>(&lhs_root->payload)
        : nullptr;
    const T* rhs = rhs_root
        ? std::get_if<T>(&rhs_root->payload)
        : nullptr;
    if (!lhs || !rhs)
        return std::nullopt;
    return *lhs < *rhs ? -1 : *lhs > *rhs ? 1 : 0;
}

[[nodiscard]] std::optional<int> CompareExactInteger(
    BuiltinType type,
    const ProgramValueGraph& lhs,
    const ProgramValueGraph& rhs)
{
    switch (type)
    {
    case BuiltinType::U8:
        return CompareExactInteger<std::uint8_t>(lhs, rhs);
    case BuiltinType::U16:
        return CompareExactInteger<std::uint16_t>(lhs, rhs);
    case BuiltinType::U32:
        return CompareExactInteger<std::uint32_t>(lhs, rhs);
    case BuiltinType::U64:
        return CompareExactInteger<std::uint64_t>(lhs, rhs);
    case BuiltinType::I32:
        return CompareExactInteger<std::int32_t>(lhs, rhs);
    case BuiltinType::I64:
        return CompareExactInteger<std::int64_t>(lhs, rhs);
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<bool> Boolean(
    const ProgramValueGraph& graph)
{
    const ProgramValue* root = RootValue(graph);
    if (!root)
        return std::nullopt;
    if (const bool* value = std::get_if<bool>(&root->payload))
        return *value;
    return std::nullopt;
}

[[nodiscard]] bool GraphEquivalent(
    const ProgramValueGraph& lhs,
    ProgramValueId lhs_id,
    const ProgramValueGraph& rhs,
    ProgramValueId rhs_id)
{
    const ProgramValue* left = FindValue(lhs, lhs_id);
    const ProgramValue* right = FindValue(rhs, rhs_id);
    if (!left || !right || left->type != right->type ||
        left->payload.index() != right->payload.index())
    {
        return false;
    }
    if (const auto* l = std::get_if<OptionalValue>(&left->payload))
    {
        const auto& r = std::get<OptionalValue>(right->payload);
        return l->value.has_value() == r.value.has_value() &&
            (!l->value ||
             GraphEquivalent(lhs, *l->value, rhs, *r.value));
    }
    if (const auto* l = std::get_if<RecordValue>(&left->payload))
    {
        const auto& r = std::get<RecordValue>(right->payload);
        if (l->fields.size() != r.fields.size())
            return false;
        for (std::size_t index = 0; index < l->fields.size(); ++index)
        {
            if (!GraphEquivalent(
                    lhs,
                    l->fields[index],
                    rhs,
                    r.fields[index]))
            {
                return false;
            }
        }
        return true;
    }
    if (const auto* l = std::get_if<ListValue>(&left->payload))
    {
        const auto& r = std::get<ListValue>(right->payload);
        if (l->elements.size() != r.elements.size())
            return false;
        for (std::size_t index = 0; index < l->elements.size(); ++index)
        {
            if (!GraphEquivalent(
                    lhs,
                    l->elements[index],
                    rhs,
                    r.elements[index]))
            {
                return false;
            }
        }
        return true;
    }
    return left->payload == right->payload;
}

} // namespace

struct ProgramExecutor::Impl
{
    struct DeferredAction
    {
        ExactDependencyIdentity action;
        ProgramValueGraph input;
    };

    struct Scope
    {
        ProgramScopeId id;
        ProgramScopeId parent;
        std::vector<DeferredAction> deferred;
    };

    struct Frame
    {
        const ProgramModule* module = nullptr;
        const ProgramFunction* function = nullptr;
        const BasicBlock* block = nullptr;
        std::size_t instruction_index = 0;
        std::unordered_map<std::uint64_t, ProgramValueGraph> values;
        std::optional<ProgramValueId> return_destination;
    };

    enum class PendingSuccess : std::uint8_t
    {
        BindValue,
        PushScope,
        CloseScope,
        Promote,
        DeferredCleanup,
        FinishInvocation,
    };

    struct PendingHost
    {
        ExecutorHostRequest request;
        PendingSuccess success = PendingSuccess::BindValue;
        std::optional<ProgramValueId> result;
        std::optional<ProgramActionRequestId> request_id;
    };

    explicit Impl(
        PureReducerInvoker reducers,
        ProgramExecutorClock clock)
        : reducers(std::move(reducers)),
          clock(std::move(clock))
    {
        if (!this->clock)
        {
            this->clock = [] {
                return std::chrono::steady_clock::now();
            };
        }
    }

    [[nodiscard]] const ProgramEntrypoint* Entrypoint() const
    {
        if (!verified || !verified->module)
            return nullptr;
        const auto found = std::find_if(
            verified->module->entrypoints.begin(),
            verified->module->entrypoints.end(),
            [this](const ProgramEntrypoint& candidate) {
                return candidate.name == invocation.entrypoint;
            });
        return found == verified->module->entrypoints.end()
            ? nullptr
            : &*found;
    }

    [[nodiscard]] const ProgramFunction* Function(
        const ProgramModule& module,
        ProgramFunctionId id) const
    {
        const auto found = std::find_if(
            module.functions.begin(),
            module.functions.end(),
            [id](const ProgramFunction& candidate) {
                return candidate.id == id;
            });
        return found == module.functions.end() ? nullptr : &*found;
    }

    [[nodiscard]] const BasicBlock* Block(
        const ProgramFunction& function,
        ProgramBlockId id) const
    {
        const auto found = std::find_if(
            function.blocks.begin(),
            function.blocks.end(),
            [id](const BasicBlock& candidate) {
                return candidate.id == id;
            });
        return found == function.blocks.end() ? nullptr : &*found;
    }

    [[nodiscard]] const ProgramValueGraph* Value(
        ProgramValueId id) const
    {
        if (frames.empty())
            return nullptr;
        const auto found = frames.back().values.find(id.value());
        if (found == frames.back().values.end())
            return nullptr;
        for (const ProgramValue& value : found->second.values)
        {
            const bool stale = std::visit(
                [this](const auto& payload) {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (
                        std::is_same_v<T, ResourceHandleValue>)
                    {
                        return payload.workset_epoch !=
                            invocation.state.expected_epoch;
                    }
                    else if constexpr (
                        std::is_same_v<T, OpaqueHandleValue>)
                    {
                        return payload.workset_epoch !=
                            invocation.state.expected_epoch;
                    }
                    return false;
                },
                value.payload);
            if (stale)
                return nullptr;
        }
        return &found->second;
    }

    [[nodiscard]] std::optional<TypeRef> DefinitionType(
        ProgramValueId id) const
    {
        if (frames.empty() || !frames.back().function)
            return std::nullopt;
        const ProgramFunction& function = *frames.back().function;
        const auto argument = std::ranges::find(
            function.arguments,
            id,
            &ValueDefinition::id);
        if (argument != function.arguments.end())
            return argument->type;
        for (const BasicBlock& block : function.blocks)
        {
            const auto block_argument = std::ranges::find(
                block.arguments,
                id,
                &ValueDefinition::id);
            if (block_argument != block.arguments.end())
                return block_argument->type;
            for (const Instruction& instruction :
                 block.instructions)
            {
                if (instruction.result &&
                    instruction.result->id == id)
                {
                    return instruction.result->type;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const TypeSchemaDefinition* Schema(
        const TypeRef& type) const
    {
        if (!type.is_named())
            return nullptr;
        if (!frames.empty() && frames.back().module)
        {
            const auto local = std::ranges::find(
                frames.back().module->local_types,
                *type.named,
                &TypeSchemaDefinition::identity);
            if (local != frames.back().module->local_types.end())
                return &*local;
        }
        if (!verified)
            return nullptr;
        const auto imported = std::ranges::find(
            verified->type_closure,
            *type.named,
            &TypeSchemaDefinition::identity);
        return imported == verified->type_closure.end()
            ? nullptr
            : &*imported;
    }

    [[nodiscard]] std::optional<std::size_t> RecordFieldIndex(
        const Instruction& instruction) const
    {
        if (instruction.operands.empty())
            return std::nullopt;
        const std::optional<TypeRef> source_type =
            DefinitionType(instruction.operands.front());
        const TypeSchemaDefinition* record =
            source_type ? Schema(*source_type) : nullptr;
        if (!record || record->kind != TypeSchemaKind::Record)
            return std::nullopt;
        const auto field = std::ranges::find(
            record->record_fields,
            instruction.selector,
            &RecordFieldDefinition::name);
        if (field == record->record_fields.end())
            return std::nullopt;
        return static_cast<std::size_t>(
            std::distance(record->record_fields.begin(), field));
    }

    [[nodiscard]] bool Bind(
        ProgramValueId id,
        const TypeRef& expected_type,
        ProgramValueGraph value)
    {
        if (!id || value.values.empty() || !RootValue(value))
            return false;
        const ProgramValueArenaLimits remaining{
            limits.maximum_values >= value_count
                ? limits.maximum_values - value_count
                : 0,
            limits.maximum_value_bytes >= value_bytes
                ? limits.maximum_value_bytes - value_bytes
                : 0,
        };
        const ProgramValueArenaStatus graph_status =
            ValidateProgramValueGraph(
                value,
                expected_type,
                verified ? std::span<
                               const TypeSchemaDefinition>(
                               verified->type_closure)
                         : std::span<
                               const TypeSchemaDefinition>{},
                remaining,
                invocation.state.expected_epoch);
        if (!graph_status)
        {
            const bool budget =
                graph_status.error ==
                    ProgramValueArenaError::ValueBudgetExceeded ||
                graph_status.error ==
                    ProgramValueArenaError::ByteBudgetExceeded;
            BeginFailure(
                budget
                    ? ProgramInfrastructureStatus::BudgetExhausted
                    : ProgramInfrastructureStatus::ContractFailed,
                budget ? "value_budget" : "invalid_value",
                graph_status.message.empty()
                    ? "Program value graph violated its verified schema"
                    : graph_status.message);
            return false;
        }
        const ProgramValueGraphMeasureResult measured =
            MeasureProgramValueGraph(value, remaining);
        if (!measured)
        {
            BeginFailure(
                ProgramInfrastructureStatus::BudgetExhausted,
                "value_budget",
                measured.status.message.empty()
                    ? "Program value arena budget was exhausted"
                    : measured.status.message);
            return false;
        }
        // The verifier guarantees one static definition for every SSA id.
        // A control-flow loop can execute that definition repeatedly; the
        // current frame must then expose the value from the latest dynamic
        // visit. Budgets remain cumulative across visits below.
        frames.back().values.insert_or_assign(
            id.value(),
            std::move(value));
        value_count += measured.value_count;
        value_bytes += measured.value_bytes;
        return true;
    }

    void Trace(
        std::string kind,
        ProgramSourceLocationId source = {},
        std::vector<ProvenanceEntry> attributes = {})
    {
        if (!invocation.execution.record_trace ||
            result.trace.size() >= limits.maximum_trace_events)
        {
            return;
        }
        result.trace.push_back(ProgramTraceEvent{
            ProgramTraceSequence(next_trace++),
            std::move(kind),
            source ? std::optional<ProgramSourceLocationId>(source)
                   : std::nullopt,
            invocation.state.expected_epoch,
            std::move(attributes)});
    }

    void Diagnostic(
        DiagnosticSeverity severity,
        std::string code,
        std::string message,
        ProgramSourceLocationId source = {})
    {
        result.diagnostics.push_back(ProgramDiagnostic{
            severity,
            std::move(code),
            std::move(message),
            source ? std::optional<ProgramSourceLocationId>(source)
                   : std::nullopt,
            {}});
    }

    void BeginFailure(
        ProgramInfrastructureStatus status,
        std::string code,
        std::string message,
        ProgramSourceLocationId source = {})
    {
        if (unwinding || activity == ProgramExecutorActivity::Terminal)
            return;
        result.infrastructure = status;
        Diagnostic(
            DiagnosticSeverity::Error,
            std::move(code),
            std::move(message),
            source);
        unwinding = true;
        explicit_scope_close = false;
        activity = ProgramExecutorActivity::Unwinding;
    }

    void BeginReturn(
        const Terminator& terminator)
    {
        if (terminator.return_value)
        {
            if (const ProgramValueGraph* value =
                    Value(*terminator.return_value))
            {
                result.output = *value;
            }
        }
        if (terminator.domain_outcome)
        {
            if (const ProgramValueGraph* value =
                    Value(*terminator.domain_outcome))
            {
                result.domain_outcome = *value;
            }
        }
        result.infrastructure = ProgramInfrastructureStatus::Completed;
        unwinding = true;
        explicit_scope_close = false;
        activity = ProgramExecutorActivity::Unwinding;
    }

    [[nodiscard]] ProgramExecutorPumpResult Suspend(
        ExecutorHostRequest request,
        PendingSuccess success,
        std::optional<ProgramValueId> result_value = {})
    {
        pending = PendingHost{
            request,
            success,
            result_value,
            {}};
        activity = ProgramExecutorActivity::AwaitingHost;
        return {false, std::move(request), {}};
    }

    [[nodiscard]] ProgramExecutorPumpResult PumpCleanup()
    {
        if (scopes.empty())
        {
            BeginFailure(
                ProgramInfrastructureStatus::ContractFailed,
                "scope_underflow",
                "Program cleanup lost the invocation scope");
            return {};
        }

        Scope& scope = scopes.back();
        if (!scope.deferred.empty())
        {
            DeferredAction deferred =
                std::move(scope.deferred.back());
            scope.deferred.pop_back();
            return Suspend(
                ExecutorHostRequest{
                    ProgramHostOperation::InvokeAction,
                    std::move(deferred.action),
                    std::move(deferred.input),
                    scope.id,
                    scope.parent,
                    {},
                    true},
                PendingSuccess::DeferredCleanup);
        }

        if (explicit_scope_close)
        {
            return Suspend(
                ExecutorHostRequest{
                    ProgramHostOperation::CloseScope,
                    {},
                    UnitGraph(),
                    scope.id,
                    scope.parent,
                    {},
                    true},
                PendingSuccess::CloseScope);
        }

        if (scopes.size() > 1)
        {
            return Suspend(
                ExecutorHostRequest{
                    ProgramHostOperation::CloseScope,
                    {},
                    UnitGraph(),
                    scope.id,
                    scope.parent,
                    {},
                    true},
                PendingSuccess::CloseScope);
        }

        return Suspend(
            ExecutorHostRequest{
                ProgramHostOperation::FinishInvocation,
                {},
                UnitGraph(),
                scope.id,
                scope.parent,
                {},
                true},
            PendingSuccess::FinishInvocation);
    }

    [[nodiscard]] bool EnterBlock(
        Frame& frame,
        const BlockEdge& edge)
    {
        const BasicBlock* target = Block(*frame.function, edge.target);
        if (!target || target->arguments.size() != edge.arguments.size())
            return false;
        std::vector<ProgramValueGraph> values;
        values.reserve(edge.arguments.size());
        for (ProgramValueId argument : edge.arguments)
        {
            const auto found = frame.values.find(argument.value());
            if (found == frame.values.end())
                return false;
            values.push_back(found->second);
        }
        frame.block = target;
        frame.instruction_index = 0;
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            frame.values[target->arguments[index].id.value()] =
                std::move(values[index]);
        }
        Trace(
            "branch",
            target->terminator.source_location,
            {{"block", std::to_string(target->id.value())}});
        return true;
    }

    [[nodiscard]] std::optional<ProgramValueGraph> BuildInput(
        const Instruction& instruction,
        TypeRef input_type)
    {
        std::vector<ProgramValueGraph> operands;
        operands.reserve(instruction.operands.size());
        for (ProgramValueId operand : instruction.operands)
        {
            const ProgramValueGraph* value = Value(operand);
            if (!value)
                return std::nullopt;
            operands.push_back(*value);
        }
        if (operands.empty())
            return UnitGraph();
        if (operands.size() == 1)
            return operands.front();
        return CompositeGraph(
            std::move(input_type),
            operands,
            false);
    }

    [[nodiscard]] const ActionDescriptor* Action(
        const ExactDependencyIdentity& identity) const
    {
        const auto found = std::find_if(
            verified->actions.begin(),
            verified->actions.end(),
            [&identity](const VerifiedActionBinding& binding) {
                return binding.import == identity;
            });
        return found == verified->actions.end()
            ? nullptr
            : &found->descriptor;
    }

    [[nodiscard]] bool PushCall(
        const ProgramModule& module,
        const ProgramFunction& function,
        const Instruction& instruction)
    {
        if (++calls_executed > limits.maximum_calls ||
            frames.size() + 1 > limits.maximum_call_depth)
        {
            BeginFailure(
                ProgramInfrastructureStatus::BudgetExhausted,
                "call_budget",
                "Program call budget was exhausted",
                instruction.source_location);
            return false;
        }
        if (function.arguments.size() != instruction.operands.size())
        {
            BeginFailure(
                ProgramInfrastructureStatus::ContractFailed,
                "call_arity",
                "Program call argument count changed after verification",
                instruction.source_location);
            return false;
        }
        Frame child;
        child.module = &module;
        child.function = &function;
        child.block = Block(function, function.entry_block);
        child.return_destination =
            instruction.result
            ? std::optional<ProgramValueId>(instruction.result->id)
            : std::nullopt;
        if (!child.block)
            return false;
        for (std::size_t index = 0;
            index < function.arguments.size();
            ++index)
        {
            const ProgramValueGraph* value =
                Value(instruction.operands[index]);
            if (!value)
                return false;
            child.values.emplace(
                function.arguments[index].id.value(),
                *value);
        }
        ++frames.back().instruction_index;
        frames.push_back(std::move(child));
        Trace(
            "call",
            instruction.source_location,
            {{"function", function.name}});
        return true;
    }

    [[nodiscard]] ProgramExecutorPumpResult ExecuteInstruction(
        const Instruction& instruction)
    {
        auto fail = [this, &instruction](std::string message) {
            BeginFailure(
                ProgramInfrastructureStatus::ContractFailed,
                "instruction_failed",
                std::move(message),
                instruction.source_location);
            return ProgramExecutorPumpResult{};
        };
        auto operand = [this](std::size_t index)
            -> const ProgramValueGraph* {
            if (index >= frames.back().block
                    ->instructions[frames.back().instruction_index]
                    .operands.size())
            {
                return nullptr;
            }
            return Value(
                frames.back().block
                    ->instructions[frames.back().instruction_index]
                    .operands[index]);
        };
        auto bind_result =
            [this, &instruction, &fail](
                ProgramValueGraph graph)
                -> ProgramExecutorPumpResult {
            if (!instruction.result ||
                !Bind(
                    instruction.result->id,
                    instruction.result->type,
                    std::move(graph)))
            {
                return fail("Instruction result could not be bound");
            }
            ++frames.back().instruction_index;
            return {true, {}, {}};
        };

        Trace(
            "instruction",
            instruction.source_location,
            {{"opcode", std::to_string(
                static_cast<std::uint16_t>(instruction.opcode))}});

        switch (instruction.opcode)
        {
        case InstructionOpcode::Constant:
            if (!instruction.literal)
                return fail("Constant has no literal");
            return bind_result(FromLiteral(*instruction.literal));
        case InstructionOpcode::Copy:
            if (!operand(0))
                return fail("Copy has no operand");
            return bind_result(*operand(0));
        case InstructionOpcode::Select:
        {
            if (!operand(0) || !operand(1) || !operand(2))
                return fail("Select is missing an operand");
            const auto condition = Boolean(*operand(0));
            if (!condition)
                return fail("Select condition is not bool");
            return bind_result(*operand(*condition ? 1 : 2));
        }
        case InstructionOpcode::CheckedConvert:
        {
            if (!instruction.result || !operand(0))
                return fail("Checked conversion is malformed");
            const Number number = ToNumber(*operand(0));
            if (!number.valid)
                return fail("Checked conversion operand is not numeric");
            std::optional<ProgramValueGraph> converted;
            if (IsIntegerBuiltin(
                    instruction.result->type.builtin))
            {
                converted = ExactIntegerConversionGraph(
                    instruction.result->type,
                    number);
            }
            else if (instruction.result->type.builtin ==
                     BuiltinType::F32)
            {
                converted =
                    ExactFloatingConversionGraph<float>(
                        instruction.result->type,
                        number);
            }
            else if (instruction.result->type.builtin ==
                     BuiltinType::F64)
            {
                converted =
                    ExactFloatingConversionGraph<double>(
                        instruction.result->type,
                        number);
            }
            if (!converted)
                return fail("Checked conversion overflowed");
            return bind_result(std::move(*converted));
        }
        case InstructionOpcode::RecordConstruct:
        case InstructionOpcode::ListConstruct:
        {
            if (!instruction.result)
                return fail("Composite construction has no result");
            std::vector<ProgramValueGraph> children;
            for (std::size_t index = 0;
                index < instruction.operands.size();
                ++index)
            {
                if (!operand(index))
                    return fail("Composite construction operand is missing");
                children.push_back(*operand(index));
            }
            return bind_result(CompositeGraph(
                instruction.result->type,
                children,
                instruction.opcode == InstructionOpcode::ListConstruct));
        }
        case InstructionOpcode::RecordProject:
        case InstructionOpcode::ListIndex:
        {
            if (!operand(0))
                return fail("Projection operand is missing");
            const ProgramValue* root = RootValue(*operand(0));
            if (!root)
                return fail("Projection root is missing");
            const std::vector<ProgramValueId>* members = nullptr;
            if (const auto* record =
                    std::get_if<RecordValue>(&root->payload))
                members = &record->fields;
            if (const auto* list =
                    std::get_if<ListValue>(&root->payload))
                members = &list->elements;
            std::uint64_t index = 0;
            if (instruction.opcode == InstructionOpcode::RecordProject)
            {
                const auto field = RecordFieldIndex(instruction);
                if (!field)
                    return fail("Record projection field is unresolved");
                index = *field;
            }
            else if (
                instruction.opcode == InstructionOpcode::ListIndex &&
                instruction.operands.size() > 1)
            {
                const Number number = ToNumber(*operand(1));
                if (!number.valid || number.floating ||
                    number.signed_integer)
                {
                    return fail("List index is not unsigned");
                }
                index = number.unsigned_value;
            }
            if (!members || index >= members->size())
                return fail("Composite projection is out of bounds");
            ProgramValueGraph projected =
                ExtractGraph(*operand(0), (*members)[index]);
            if (projected.values.empty())
                return fail("Composite projection could not be extracted");
            return bind_result(std::move(projected));
        }
        case InstructionOpcode::RecordUpdate:
        {
            if (!operand(0) || !operand(1) || !instruction.result)
                return fail("Record update is malformed");
            const ProgramValue* root = RootValue(*operand(0));
            const auto* record =
                root ? std::get_if<RecordValue>(&root->payload)
                     : nullptr;
            const auto field = RecordFieldIndex(instruction);
            if (!record || !field || *field >= record->fields.size())
                return fail("Record update field is out of bounds");
            std::vector<ProgramValueGraph> fields;
            fields.reserve(record->fields.size());
            for (std::size_t index = 0;
                index < record->fields.size();
                ++index)
            {
                fields.push_back(
                    index == *field
                    ? *operand(1)
                    : ExtractGraph(
                        *operand(0),
                        record->fields[index]));
            }
            return bind_result(CompositeGraph(
                instruction.result->type,
                fields,
                false));
        }
        case InstructionOpcode::OptionalConstruct:
            if (!instruction.result)
                return fail("Optional construction has no result");
            return bind_result(OptionalGraph(
                instruction.result->type,
                instruction.operands.empty() ? nullptr : operand(0)));
        case InstructionOpcode::OptionalIsPresent:
        {
            if (!operand(0))
                return fail("Optional test has no operand");
            const ProgramValue* root = RootValue(*operand(0));
            const auto* optional =
                root ? std::get_if<OptionalValue>(&root->payload)
                     : nullptr;
            if (!optional)
                return fail("Optional test operand is not optional");
            return bind_result(ScalarGraph(
                TypeRef::Builtin(BuiltinType::Bool),
                optional->value.has_value()));
        }
        case InstructionOpcode::OptionalExtract:
        {
            if (!operand(0))
                return fail("Optional extraction has no operand");
            const ProgramValue* root = RootValue(*operand(0));
            const auto* optional =
                root ? std::get_if<OptionalValue>(&root->payload)
                     : nullptr;
            if (!optional || !optional->value)
                return fail("Required optional value is unavailable");
            return bind_result(
                ExtractGraph(*operand(0), *optional->value));
        }
        case InstructionOpcode::ListAppend:
        {
            if (!operand(0) || !operand(1) || !instruction.result)
                return fail("List append is malformed");
            const ProgramValue* root = RootValue(*operand(0));
            const auto* list =
                root ? std::get_if<ListValue>(&root->payload)
                     : nullptr;
            if (!list)
                return fail("List append target is not a list");
            std::vector<ProgramValueGraph> elements;
            for (ProgramValueId element : list->elements)
                elements.push_back(ExtractGraph(*operand(0), element));
            elements.push_back(*operand(1));
            return bind_result(CompositeGraph(
                instruction.result->type,
                elements,
                true));
        }
        case InstructionOpcode::ListSize:
        {
            if (!operand(0))
                return fail("List size has no operand");
            const ProgramValue* root = RootValue(*operand(0));
            const auto* list =
                root ? std::get_if<ListValue>(&root->payload)
                     : nullptr;
            if (!list)
                return fail("List size target is not a list");
            return bind_result(ScalarGraph(
                instruction.result
                    ? instruction.result->type
                    : TypeRef::Builtin(BuiltinType::U64),
                static_cast<std::uint64_t>(list->elements.size())));
        }
        case InstructionOpcode::AddChecked:
        case InstructionOpcode::SubtractChecked:
        case InstructionOpcode::MultiplyChecked:
        case InstructionOpcode::DivideChecked:
        case InstructionOpcode::RemainderChecked:
        {
            if (!operand(0) || !operand(1) || !instruction.result)
                return fail("Numeric operation is malformed");
            const Number lhs = ToNumber(*operand(0));
            const Number rhs = ToNumber(*operand(1));
            if (!lhs.valid || !rhs.valid)
                return fail("Numeric operand is invalid");
            if (IsIntegerBuiltin(
                    instruction.result->type.builtin))
            {
                auto output = CheckedIntegerBinaryGraph(
                    instruction.result->type,
                    *operand(0),
                    *operand(1),
                    instruction.opcode);
                if (!output)
                {
                    return fail(
                        instruction.opcode ==
                                InstructionOpcode::DivideChecked &&
                                rhs.unsigned_value == 0 &&
                                rhs.signed_value == 0
                            ? "Division by zero"
                            : "Checked numeric operation overflowed");
                }
                return bind_result(std::move(*output));
            }
            long double value = 0;
            switch (instruction.opcode)
            {
            case InstructionOpcode::AddChecked:
                value = lhs.floating_value + rhs.floating_value;
                break;
            case InstructionOpcode::SubtractChecked:
                value = lhs.floating_value - rhs.floating_value;
                break;
            case InstructionOpcode::MultiplyChecked:
                value = lhs.floating_value * rhs.floating_value;
                break;
            case InstructionOpcode::DivideChecked:
                if (rhs.floating_value == 0)
                    return fail("Division by zero");
                value = lhs.floating_value / rhs.floating_value;
                break;
            case InstructionOpcode::RemainderChecked:
                if (rhs.floating_value == 0 ||
                    lhs.floating || rhs.floating)
                {
                    return fail("Invalid remainder operation");
                }
                value = std::fmod(
                    lhs.floating_value,
                    rhs.floating_value);
                break;
            default:
                break;
            }
            auto output = NumberGraph(instruction.result->type, value);
            if (!output)
                return fail("Checked numeric operation overflowed");
            return bind_result(std::move(*output));
        }
        case InstructionOpcode::NegateChecked:
        {
            if (!operand(0) || !instruction.result)
                return fail("Negation is malformed");
            const Number value = ToNumber(*operand(0));
            auto output =
                value.valid &&
                    IsIntegerBuiltin(
                        instruction.result->type.builtin)
                ? CheckedIntegerNegationGraph(
                      instruction.result->type,
                      *operand(0))
                : value.valid
                ? NumberGraph(
                      instruction.result->type,
                      -value.floating_value)
                : std::nullopt;
            if (!output)
                return fail("Checked negation overflowed");
            return bind_result(std::move(*output));
        }
        case InstructionOpcode::Equal:
        case InstructionOpcode::NotEqual:
        case InstructionOpcode::Less:
        case InstructionOpcode::LessEqual:
        case InstructionOpcode::Greater:
        case InstructionOpcode::GreaterEqual:
        {
            if (!operand(0) || !operand(1))
                return fail("Comparison is malformed");
            bool compared = false;
            if (instruction.opcode == InstructionOpcode::Equal ||
                instruction.opcode == InstructionOpcode::NotEqual)
            {
                compared = GraphEquivalent(
                    *operand(0),
                    operand(0)->root,
                    *operand(1),
                    operand(1)->root);
                if (instruction.opcode == InstructionOpcode::NotEqual)
                    compared = !compared;
            }
            else
            {
                const Number lhs = ToNumber(*operand(0));
                const Number rhs = ToNumber(*operand(1));
                if (!lhs.valid || !rhs.valid)
                    return fail("Ordered comparison requires numbers");
                const ProgramValue* lhs_root =
                    RootValue(*operand(0));
                const std::optional<int> exact =
                    lhs_root &&
                        IsIntegerBuiltin(lhs_root->type.builtin)
                    ? CompareExactInteger(
                          lhs_root->type.builtin,
                          *operand(0),
                          *operand(1))
                    : std::nullopt;
                switch (instruction.opcode)
                {
                case InstructionOpcode::Less:
                    compared = exact
                        ? *exact < 0
                        : lhs.floating_value <
                            rhs.floating_value;
                    break;
                case InstructionOpcode::LessEqual:
                    compared = exact
                        ? *exact <= 0
                        : lhs.floating_value <=
                            rhs.floating_value;
                    break;
                case InstructionOpcode::Greater:
                    compared = exact
                        ? *exact > 0
                        : lhs.floating_value >
                            rhs.floating_value;
                    break;
                case InstructionOpcode::GreaterEqual:
                    compared = exact
                        ? *exact >= 0
                        : lhs.floating_value >=
                            rhs.floating_value;
                    break;
                default:
                    break;
                }
            }
            return bind_result(ScalarGraph(
                TypeRef::Builtin(BuiltinType::Bool),
                compared));
        }
        case InstructionOpcode::BooleanAnd:
        case InstructionOpcode::BooleanOr:
        {
            if (!operand(0) || !operand(1))
                return fail("Boolean operation is malformed");
            const auto lhs = Boolean(*operand(0));
            const auto rhs = Boolean(*operand(1));
            if (!lhs || !rhs)
                return fail("Boolean operation requires bool operands");
            return bind_result(ScalarGraph(
                TypeRef::Builtin(BuiltinType::Bool),
                instruction.opcode == InstructionOpcode::BooleanAnd
                    ? (*lhs && *rhs)
                    : (*lhs || *rhs)));
        }
        case InstructionOpcode::BooleanNot:
        {
            if (!operand(0))
                return fail("Boolean not has no operand");
            const auto value = Boolean(*operand(0));
            if (!value)
                return fail("Boolean not requires bool");
            return bind_result(ScalarGraph(
                TypeRef::Builtin(BuiltinType::Bool),
                !*value));
        }
        case InstructionOpcode::CallLocal:
        {
            const ProgramFunction* function =
                Function(*frames.back().module,
                         instruction.target.local_function);
            if (!function ||
                !PushCall(*frames.back().module, *function, instruction))
            {
                return fail("Local call target is unavailable");
            }
            return {true, {}, {}};
        }
        case InstructionOpcode::CallImported:
        {
            if (!instruction.target.dependency)
                return fail("Imported call has no exact dependency");
            const ProgramModule* module = nullptr;
            for (const auto& candidate : verified->module_closure)
            {
                if (candidate->identity.canonical_id ==
                        instruction.target.dependency->canonical_id &&
                    candidate->identity.revision ==
                        instruction.target.dependency->version &&
                    candidate->identity.module_hash ==
                        instruction.target.dependency->signature_hash)
                {
                    module = candidate.get();
                    break;
                }
            }
            if (!module)
                return fail("Imported module is unavailable");
            const auto found = std::find_if(
                module->functions.begin(),
                module->functions.end(),
                [&instruction](const ProgramFunction& candidate) {
                    return candidate.name ==
                        instruction.target.member_name;
                });
            if (found == module->functions.end() ||
                !PushCall(*module, *found, instruction))
            {
                return fail("Imported function is unavailable");
            }
            return {true, {}, {}};
        }
        case InstructionOpcode::CallReducer:
        {
            if (!instruction.target.dependency || !instruction.result ||
                !reducers)
            {
                return fail("Reducer call is unavailable");
            }
            std::vector<ProgramValueGraph> inputs;
            for (std::size_t index = 0;
                index < instruction.operands.size();
                ++index)
            {
                if (!operand(index))
                    return fail("Reducer operand is unavailable");
                inputs.push_back(*operand(index));
            }
            std::string diagnostic;
            auto output = reducers(
                *instruction.target.dependency,
                inputs,
                diagnostic);
            if (!output)
            {
                return fail(
                    diagnostic.empty()
                        ? "Pure reducer failed"
                        : std::move(diagnostic));
            }
            return bind_result(std::move(*output));
        }
        case InstructionOpcode::AwaitAction:
        {
            if (!instruction.target.dependency)
                return fail("Action await has no exact dependency");
            const ActionDescriptor* descriptor =
                Action(*instruction.target.dependency);
            if (!descriptor)
                return fail("Action await import is unavailable");
            auto input = BuildInput(
                instruction,
                descriptor->input_type);
            if (!input)
                return fail("Action input could not be constructed");
            if (++action_requests > limits.maximum_action_requests)
            {
                BeginFailure(
                    ProgramInfrastructureStatus::BudgetExhausted,
                    "action_budget",
                    "Program action request budget was exhausted",
                    instruction.source_location);
                return {};
            }
            ++frames.back().instruction_index;
            return Suspend(
                ExecutorHostRequest{
                    ProgramHostOperation::InvokeAction,
                    *instruction.target.dependency,
                    std::move(*input),
                    scopes.back().id,
                    scopes.back().parent,
                    {},
                    false},
                PendingSuccess::BindValue,
                instruction.result
                    ? std::optional<ProgramValueId>(
                        instruction.result->id)
                    : std::nullopt);
        }
        case InstructionOpcode::EmitRecord:
        {
            if (!operand(0) ||
                ++emissions > limits.maximum_emissions)
            {
                return fail("Emission is unavailable or over budget");
            }
            const ProgramValue* root = RootValue(*operand(0));
            if (!root || !root->type.named)
                return fail("Emission requires a named record value");
            result.emissions.push_back(ProgramEmission{
                ProgramEmissionSequence(next_emission++),
                *root->type.named,
                *operand(0),
                true});
            ++frames.back().instruction_index;
            return {true, {}, {}};
        }
        case InstructionOpcode::PublishArtifact:
        {
            if (!operand(0) ||
                ++artifacts > limits.maximum_artifacts)
            {
                return fail("Artifact is unavailable or over budget");
            }
            const ProgramValue* root = RootValue(*operand(0));
            const auto* artifact = root
                ? std::get_if<ArtifactReferenceValue>(&root->payload)
                : nullptr;
            if (!artifact || !artifact->complete)
                return fail("Only complete artifacts may be published");
            result.artifacts.push_back(ProgramArtifact{
                ProgramArtifactSequence(next_artifact++),
                *artifact});
            ++frames.back().instruction_index;
            return {true, {}, {}};
        }
        case InstructionOpcode::EnterScope:
        {
            if (!instruction.scope || scopes.empty())
                return fail("Scope entry is malformed");
            ++frames.back().instruction_index;
            return Suspend(
                ExecutorHostRequest{
                    ProgramHostOperation::OpenScope,
                    {},
                    UnitGraph(),
                    instruction.scope,
                    scopes.back().id,
                    {},
                    false},
                PendingSuccess::PushScope);
        }
        case InstructionOpcode::ExitScope:
        {
            if (scopes.size() <= 1 ||
                scopes.back().id != instruction.scope)
            {
                return fail("Scope exit is not properly nested");
            }
            ++frames.back().instruction_index;
            explicit_scope_close = true;
            activity = ProgramExecutorActivity::Unwinding;
            return {true, {}, {}};
        }
        case InstructionOpcode::DeferCompensation:
        {
            if (!instruction.target.dependency || scopes.empty())
                return fail("Deferred compensation is malformed");
            const ActionDescriptor* descriptor =
                Action(*instruction.target.dependency);
            if (!descriptor)
                return fail("Deferred action import is unavailable");
            auto input = BuildInput(instruction, descriptor->input_type);
            if (!input)
                return fail("Deferred action input is unavailable");
            // Reserve the cleanup action budget when the defer is created.
            // Once unwind begins, mandatory cleanup cannot be skipped merely
            // because the ordinary action budget has been exhausted.
            if (++action_requests > limits.maximum_action_requests)
            {
                BeginFailure(
                    ProgramInfrastructureStatus::BudgetExhausted,
                    "action_budget",
                    "Program deferred-action budget was exhausted",
                    instruction.source_location);
                return {};
            }
            scopes.back().deferred.push_back(DeferredAction{
                *instruction.target.dependency,
                std::move(*input)});
            ++frames.back().instruction_index;
            return {true, {}, {}};
        }
        case InstructionOpcode::PromoteResource:
        {
            if (!operand(0))
                return fail("Resource promotion has no handle");
            const ProgramValue* root = RootValue(*operand(0));
            const auto* handle = root
                ? std::get_if<ResourceHandleValue>(&root->payload)
                : nullptr;
            if (!handle)
                return fail("Resource promotion operand is not a handle");
            ++frames.back().instruction_index;
            return Suspend(
                ExecutorHostRequest{
                    ProgramHostOperation::PromoteResource,
                    {},
                    UnitGraph(),
                    instruction.scope,
                    scopes.back().id,
                    handle->handle_id,
                    false},
                PendingSuccess::Promote);
        }
        }
        return fail("Unknown canonical instruction");
    }

    [[nodiscard]] ProgramExecutorPumpResult ExecuteTerminator(
        const Terminator& terminator)
    {
        switch (terminator.kind)
        {
        case TerminatorKind::Branch:
            if (terminator.edges.size() != 1 ||
                !EnterBlock(frames.back(), terminator.edges.front()))
            {
                BeginFailure(
                    ProgramInfrastructureStatus::ContractFailed,
                    "invalid_branch",
                    "Branch target became invalid after verification",
                    terminator.source_location);
            }
            return {true, {}, {}};
        case TerminatorKind::ConditionalBranch:
        {
            if (!terminator.condition_or_selector ||
                terminator.edges.size() != 2)
            {
                BeginFailure(
                    ProgramInfrastructureStatus::ContractFailed,
                    "invalid_branch",
                    "Conditional branch is malformed",
                    terminator.source_location);
                return {};
            }
            const ProgramValueGraph* condition =
                Value(*terminator.condition_or_selector);
            const auto value =
                condition ? Boolean(*condition) : std::nullopt;
            if (!value ||
                !EnterBlock(
                    frames.back(),
                    terminator.edges[*value ? 0 : 1]))
            {
                BeginFailure(
                    ProgramInfrastructureStatus::ContractFailed,
                    "invalid_branch",
                    "Conditional branch could not select its edge",
                    terminator.source_location);
            }
            return {true, {}, {}};
        }
        case TerminatorKind::EnumSwitch:
        {
            if (!terminator.condition_or_selector)
            {
                BeginFailure(
                    ProgramInfrastructureStatus::ContractFailed,
                    "invalid_switch",
                    "Enum switch has no selector",
                    terminator.source_location);
                return {};
            }
            const ProgramValueGraph* graph =
                Value(*terminator.condition_or_selector);
            const ProgramValue* root =
                graph ? RootValue(*graph) : nullptr;
            const auto* value = root
                ? std::get_if<EnumValue>(&root->payload)
                : nullptr;
            const BlockEdge* selected = nullptr;
            if (value)
            {
                const auto found = std::find_if(
                    terminator.enum_cases.begin(),
                    terminator.enum_cases.end(),
                    [value](const EnumSwitchCase& candidate) {
                        return candidate.enum_value == value->value;
                    });
                if (found != terminator.enum_cases.end())
                    selected = &found->edge;
            }
            if (!selected && terminator.default_edge)
                selected = &*terminator.default_edge;
            if (!selected ||
                !EnterBlock(frames.back(), *selected))
            {
                BeginFailure(
                    ProgramInfrastructureStatus::ContractFailed,
                    "invalid_switch",
                    "Enum switch has no matching edge",
                    terminator.source_location);
            }
            return {true, {}, {}};
        }
        case TerminatorKind::Return:
        {
            ProgramValueGraph returned;
            if (terminator.return_value)
            {
                const ProgramValueGraph* value =
                    Value(*terminator.return_value);
                if (!value)
                {
                    BeginFailure(
                        ProgramInfrastructureStatus::ContractFailed,
                        "missing_return",
                        "Return value is unavailable",
                        terminator.source_location);
                    return {};
                }
                returned = *value;
            }
            if (frames.size() > 1)
            {
                const auto destination =
                    frames.back().return_destination;
                frames.pop_back();
                const auto expected = destination
                    ? DefinitionType(*destination)
                    : std::nullopt;
                if (destination &&
                    (!expected ||
                     !Bind(
                         *destination,
                         *expected,
                         std::move(returned))))
                {
                    return {};
                }
                return {true, {}, {}};
            }
            BeginReturn(terminator);
            return {true, {}, {}};
        }
        case TerminatorKind::StructuredFail:
            BeginFailure(
                ProgramInfrastructureStatus::ContractFailed,
                terminator.failure
                    ? terminator.failure->code
                    : "structured_failure",
                terminator.failure
                    ? terminator.failure->message
                    : "Program reported a structured failure",
                terminator.source_location);
            return {true, {}, {}};
        }
        return {};
    }

    PureReducerInvoker reducers;
    ProgramExecutorClock clock;
    std::shared_ptr<const VerifiedProgramModule> verified;
    ProgramInvocation invocation;
    CancellationToken cancellation;
    ProgramBudgets limits;
    ProgramResult result;
    ProgramExecutorActivity activity = ProgramExecutorActivity::Idle;
    std::vector<Frame> frames;
    std::vector<Scope> scopes;
    std::optional<PendingHost> pending;
    bool unwinding = false;
    bool explicit_scope_close = false;
    CancellationReason explicit_cancellation = CancellationReason::None;
    std::uint64_t instructions_executed = 0;
    std::uint64_t calls_executed = 0;
    std::uint64_t action_requests = 0;
    std::uint64_t emissions = 0;
    std::uint64_t artifacts = 0;
    std::uint64_t value_count = 0;
    std::uint64_t value_bytes = 0;
    std::uint64_t next_trace = 1;
    std::uint64_t next_emission = 1;
    std::uint64_t next_artifact = 1;
};

ProgramExecutor::ProgramExecutor(
    PureReducerInvoker reducers,
    ProgramExecutorClock clock)
    : impl_(std::make_unique<Impl>(
          std::move(reducers),
          std::move(clock)))
{
}

ProgramExecutor::~ProgramExecutor() = default;
ProgramExecutor::ProgramExecutor(ProgramExecutor&&) noexcept = default;
ProgramExecutor& ProgramExecutor::operator=(ProgramExecutor&&) noexcept =
    default;

bool ProgramExecutor::Start(
    std::shared_ptr<const VerifiedProgramModule> verified,
    ProgramInvocation invocation,
    CancellationToken cancellation,
    std::string* diagnostic)
{
    if (impl_->activity != ProgramExecutorActivity::Idle ||
        !verified || !verified->module ||
        !invocation.invocation_id || !invocation.attempt_id ||
        !invocation.state.expected_epoch)
    {
        if (diagnostic)
            *diagnostic = "ProgramExecutor start request is invalid";
        return false;
    }
    impl_->verified = std::move(verified);
    impl_->invocation = std::move(invocation);
    impl_->cancellation = std::move(cancellation);
    impl_->limits = impl_->invocation.limits;
    if (impl_->limits.maximum_instructions == 0 ||
        impl_->limits.maximum_calls == 0 ||
        impl_->limits.maximum_call_depth == 0 ||
        impl_->limits.maximum_action_requests == 0 ||
        impl_->limits.maximum_values == 0 ||
        impl_->limits.maximum_value_bytes == 0 ||
        impl_->limits.maximum_trace_events == 0)
    {
        if (diagnostic)
            *diagnostic = "ProgramExecutor requires finite nonzero budgets";
        return false;
    }

    const ProgramEntrypoint* entrypoint = impl_->Entrypoint();
    const ProgramFunction* function = entrypoint
        ? impl_->Function(*impl_->verified->module,
                          entrypoint->function)
        : nullptr;
    if (!entrypoint || !function ||
        function->arguments.size() != 1)
    {
        if (diagnostic)
            *diagnostic =
                "ProgramExecutor entrypoint requires one typed input";
        return false;
    }
    const BasicBlock* block =
        impl_->Block(*function, function->entry_block);
    if (!block || impl_->invocation.input.values.empty() ||
        !RootValue(impl_->invocation.input))
    {
        if (diagnostic)
            *diagnostic =
                "ProgramExecutor entrypoint or input graph is invalid";
        return false;
    }
    const ProgramValueArenaStatus input_status =
        ValidateProgramValueGraph(
            impl_->invocation.input,
            entrypoint->input_type,
            impl_->verified->type_closure,
            {
                impl_->limits.maximum_values,
                impl_->limits.maximum_value_bytes,
            },
            impl_->invocation.state.expected_epoch);
    if (!input_status)
    {
        if (diagnostic)
        {
            *diagnostic = input_status.message.empty()
                ? "ProgramExecutor input graph violates its verified schema"
                : input_status.message;
        }
        return false;
    }
    const ProgramValueGraphMeasureResult input_measure =
        MeasureProgramValueGraph(
            impl_->invocation.input,
            {
                impl_->limits.maximum_values,
                impl_->limits.maximum_value_bytes,
            });
    if (!input_measure)
    {
        if (diagnostic)
        {
            *diagnostic = input_measure.status.message.empty()
                ? "ProgramExecutor input graph exceeds its value budget"
                : input_measure.status.message;
        }
        return false;
    }

    impl_->result = ProgramResult{
        .invocation_id = impl_->invocation.invocation_id,
        .attempt_id = impl_->invocation.attempt_id,
        .module = impl_->invocation.module,
        .entrypoint = impl_->invocation.entrypoint,
        .resolved_dependencies =
            impl_->verified->dependency_lock,
        .infrastructure =
            ProgramInfrastructureStatus::ContractFailed,
        .cleanup = ProgramCleanupStatus::Clean,
        .session_disposition = SessionDisposition::Clean,
        .provenance = impl_->invocation.provenance,
    };
    Impl::Frame frame;
    frame.module = impl_->verified->module.get();
    frame.function = function;
    frame.block = block;
    frame.values.emplace(
        function->arguments.front().id.value(),
        impl_->invocation.input);
    impl_->value_count = input_measure.value_count;
    impl_->value_bytes = input_measure.value_bytes;
    impl_->frames.push_back(std::move(frame));
    impl_->scopes.push_back(
        Impl::Scope{kInvocationScope, {}, {}});
    impl_->activity = ProgramExecutorActivity::Runnable;
    impl_->Trace("invocation_started");
    return true;
}

ProgramExecutorPumpResult ProgramExecutor::Pump(
    std::uint64_t maximum_instructions)
{
    if (impl_->activity == ProgramExecutorActivity::Terminal)
        return {false, {}, impl_->result};
    if (impl_->activity == ProgramExecutorActivity::AwaitingHost ||
        impl_->activity == ProgramExecutorActivity::Idle)
    {
        return {};
    }
    if (maximum_instructions == 0)
        return {true, {}, {}};

    const CancellationReason cancellation =
        impl_->explicit_cancellation != CancellationReason::None
        ? impl_->explicit_cancellation
        : impl_->cancellation.reason();
    if (cancellation != CancellationReason::None &&
        !impl_->unwinding)
    {
        impl_->BeginFailure(
            ProgramInfrastructureStatus::Cancelled,
            "cancelled",
            "Program invocation was cancelled");
    }

    std::uint64_t quantum = 0;
    while (quantum < maximum_instructions)
    {
        if (impl_->activity == ProgramExecutorActivity::Unwinding)
            return impl_->PumpCleanup();
        if (impl_->activity != ProgramExecutorActivity::Runnable ||
            impl_->frames.empty())
            return {};
        if (++impl_->instructions_executed >
            impl_->limits.maximum_instructions)
        {
            impl_->BeginFailure(
                ProgramInfrastructureStatus::BudgetExhausted,
                "instruction_budget",
                "Program instruction budget was exhausted");
            continue;
        }
        ++quantum;
        Impl::Frame& frame = impl_->frames.back();
        if (!frame.block)
        {
            impl_->BeginFailure(
                ProgramInfrastructureStatus::ContractFailed,
                "missing_block",
                "Program frame has no active basic block");
            continue;
        }
        if (frame.instruction_index < frame.block->instructions.size())
        {
            ProgramExecutorPumpResult step =
                impl_->ExecuteInstruction(
                    frame.block
                        ->instructions[frame.instruction_index]);
            if (step.host_request || step.terminal)
                return step;
            continue;
        }
        ProgramExecutorPumpResult step =
            impl_->ExecuteTerminator(frame.block->terminator);
        if (step.host_request || step.terminal)
            return step;
    }
    return {
        impl_->activity == ProgramExecutorActivity::Runnable ||
            impl_->activity == ProgramExecutorActivity::Unwinding,
        {},
        {}};
}

bool ProgramExecutor::BindPendingAction(
    ProgramActionRequestId request_id)
{
    if (!request_id ||
        impl_->activity != ProgramExecutorActivity::AwaitingHost ||
        !impl_->pending || impl_->pending->request_id)
    {
        return false;
    }
    impl_->pending->request_id = request_id;
    return true;
}

bool ProgramExecutor::DeliverHostCompletion(
    ProgramActionResolution completion,
    std::string* diagnostic)
{
    if (impl_->activity != ProgramExecutorActivity::AwaitingHost ||
        !impl_->pending || !impl_->pending->request_id ||
        completion.request_id != *impl_->pending->request_id ||
        completion.invocation_id != impl_->invocation.invocation_id ||
        completion.attempt_id != impl_->invocation.attempt_id ||
        completion.operation != impl_->pending->request.operation)
    {
        if (diagnostic)
            *diagnostic =
                "Host completion does not match the pending action";
        return false;
    }

    if (!completion.workset_epoch ||
        completion.workset_epoch !=
            impl_->invocation.state.expected_epoch)
    {
        if (diagnostic)
        {
            *diagnostic =
                "Host completion did not preserve the pending action's "
                "authoritative WorksetEpoch";
        }
        return false;
    }

    const ActionDescriptor* pending_action = nullptr;
    if (impl_->pending->request.action)
    {
        pending_action =
            impl_->Action(*impl_->pending->request.action);
        if (!pending_action)
        {
            if (diagnostic)
                *diagnostic = "Pending action binding disappeared";
            return false;
        }
    }
    std::optional<std::string> output_budget_failure;
    if (completion.status ==
            ProgramActionResolutionStatus::Completed &&
        impl_->pending->request.operation ==
            ProgramHostOperation::InvokeAction)
    {
        const ProgramValueArenaLimits remaining{
            impl_->limits.maximum_values >= impl_->value_count
                ? impl_->limits.maximum_values - impl_->value_count
                : 0,
            impl_->limits.maximum_value_bytes >= impl_->value_bytes
                ? impl_->limits.maximum_value_bytes - impl_->value_bytes
                : 0,
        };
        const ProgramValueArenaStatus output_status =
            pending_action
            ? ValidateProgramValueGraph(
                  completion.output,
                  pending_action->output_type,
                  impl_->verified->type_closure,
                  remaining,
                  completion.workset_epoch)
            : ProgramValueArenaStatus{
                  ProgramValueArenaError::SchemaMismatch,
                  "Pending action binding disappeared"};
        if (!output_status)
        {
            const bool budget =
                output_status.error ==
                    ProgramValueArenaError::ValueBudgetExceeded ||
                output_status.error ==
                    ProgramValueArenaError::ByteBudgetExceeded;
            if (budget)
            {
                output_budget_failure = output_status.message.empty()
                    ? "Action completion output exceeds the remaining "
                      "program value budget"
                    : output_status.message;
            }
            else
            {
                if (diagnostic)
                {
                    *diagnostic = output_status.message.empty()
                        ? "Action completion output does not match the "
                          "verified action contract"
                        : output_status.message;
                }
                return false;
            }
        }
    }

    Impl::PendingHost pending = std::move(*impl_->pending);
    impl_->pending.reset();
    if (completion.cleanup > impl_->result.cleanup)
        impl_->result.cleanup = completion.cleanup;
    if (completion.session_disposition >
        impl_->result.session_disposition)
    {
        impl_->result.session_disposition =
            completion.session_disposition;
    }
    for (const CleanupReceipt& receipt :
         completion.cleanup_receipts)
    {
        if (receipt.status > impl_->result.cleanup)
            impl_->result.cleanup = receipt.status;
        if (receipt.status == ProgramCleanupStatus::Tainted)
        {
            impl_->result.session_disposition =
                SessionDisposition::Tainted;
        }
    }
    impl_->result.cleanup_receipts.insert(
        impl_->result.cleanup_receipts.end(),
        std::make_move_iterator(
            completion.cleanup_receipts.begin()),
        std::make_move_iterator(
            completion.cleanup_receipts.end()));

    if (output_budget_failure)
    {
        impl_->activity = ProgramExecutorActivity::Runnable;
        impl_->BeginFailure(
            ProgramInfrastructureStatus::BudgetExhausted,
            "value_budget",
            std::move(*output_budget_failure));
        return true;
    }

    const bool success =
        completion.status ==
        ProgramActionResolutionStatus::Completed;
    if (!success)
    {
        if (pending.success == Impl::PendingSuccess::DeferredCleanup ||
            pending.success == Impl::PendingSuccess::CloseScope ||
            pending.success == Impl::PendingSuccess::FinishInvocation)
        {
            impl_->result.cleanup = ProgramCleanupStatus::Tainted;
            impl_->result.session_disposition =
                SessionDisposition::Tainted;
            impl_->Diagnostic(
                DiagnosticSeverity::Error,
                completion.code.empty()
                    ? "cleanup_failed"
                    : completion.code,
                completion.message.empty()
                    ? "Program cleanup action failed"
                    : completion.message);
            if (pending.success ==
                Impl::PendingSuccess::FinishInvocation)
            {
                impl_->result.infrastructure =
                    ProgramInfrastructureStatus::BackendFailed;
                impl_->activity = ProgramExecutorActivity::Terminal;
                return true;
            }
            if (pending.success ==
                Impl::PendingSuccess::CloseScope)
            {
                // A failed close has already established that the session is
                // not reusable. Retire the logical scope exactly once so
                // unwind cannot resubmit the same failing close forever; the
                // session-level ledger remains authoritative for best-effort
                // shutdown of any concrete resources it still owns.
                if (!impl_->scopes.empty())
                    impl_->scopes.pop_back();
                impl_->explicit_scope_close = false;
                if (!impl_->unwinding)
                {
                    impl_->unwinding = true;
                    impl_->result.infrastructure =
                        ProgramInfrastructureStatus::BackendFailed;
                }
            }
            impl_->activity = ProgramExecutorActivity::Unwinding;
            return true;
        }

        ProgramInfrastructureStatus status =
            completion.status ==
                    ProgramActionResolutionStatus::Cancelled
            ? ProgramInfrastructureStatus::Cancelled
            : completion.status ==
                      ProgramActionResolutionStatus::TimedOut
            ? ProgramInfrastructureStatus::TimedOut
            : completion.status ==
                    ProgramActionResolutionStatus::StaleEpoch
            ? ProgramInfrastructureStatus::ContractFailed
            : ProgramInfrastructureStatus::BackendFailed;
        impl_->activity = ProgramExecutorActivity::Runnable;
        impl_->BeginFailure(
            status,
            completion.code.empty()
                ? "action_failed"
                : completion.code,
            completion.message.empty()
                ? "Program action failed"
                : completion.message);
        return true;
    }

    switch (pending.success)
    {
    case Impl::PendingSuccess::BindValue:
        impl_->activity = ProgramExecutorActivity::Runnable;
        if (pending.result && pending_action &&
            !impl_->Bind(
                *pending.result,
                pending_action->output_type,
                std::move(completion.output)))
        {
            return false;
        }
        if (pending.result && !pending_action)
            return false;
        break;
    case Impl::PendingSuccess::PushScope:
        impl_->scopes.push_back(Impl::Scope{
            pending.request.scope,
            pending.request.parent_scope,
            {}});
        impl_->activity = ProgramExecutorActivity::Runnable;
        break;
    case Impl::PendingSuccess::CloseScope:
        if (!impl_->scopes.empty())
            impl_->scopes.pop_back();
        if (impl_->explicit_scope_close)
        {
            impl_->explicit_scope_close = false;
            impl_->activity = ProgramExecutorActivity::Runnable;
        }
        else
        {
            impl_->activity = ProgramExecutorActivity::Unwinding;
        }
        break;
    case Impl::PendingSuccess::Promote:
        impl_->activity = ProgramExecutorActivity::Runnable;
        break;
    case Impl::PendingSuccess::DeferredCleanup:
        impl_->activity = ProgramExecutorActivity::Unwinding;
        break;
    case Impl::PendingSuccess::FinishInvocation:
        impl_->Trace("invocation_terminal");
        impl_->activity = ProgramExecutorActivity::Terminal;
        break;
    }
    return true;
}

bool ProgramExecutor::RequestCancellation(
    CancellationReason reason) noexcept
{
    if (reason == CancellationReason::None ||
        impl_->activity == ProgramExecutorActivity::Idle ||
        impl_->activity == ProgramExecutorActivity::Terminal ||
        impl_->explicit_cancellation != CancellationReason::None)
    {
        return false;
    }
    impl_->explicit_cancellation = reason;
    return true;
}

ProgramExecutorSnapshot ProgramExecutor::snapshot() const noexcept
{
    return {
        impl_->activity,
        impl_->invocation.invocation_id,
        impl_->invocation.state.expected_epoch,
        impl_->instructions_executed,
        impl_->calls_executed,
        impl_->action_requests,
        impl_->emissions,
        impl_->artifacts,
        impl_->frames.size(),
        impl_->scopes.size(),
        impl_->pending ? impl_->pending->request_id : std::nullopt};
}

std::optional<std::chrono::steady_clock::time_point>
ProgramExecutor::next_wake() const noexcept
{
    if (impl_->activity == ProgramExecutorActivity::Terminal ||
        impl_->activity == ProgramExecutorActivity::Idle)
    {
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace savor::runtime::program
