#pragma once

#include <compare>
#include <cstdint>
#include <variant>

namespace savor::db::execution::workflow {

template <typename Tag>
class StrongWorkflowId {
public:
    constexpr StrongWorkflowId() = default;
    explicit constexpr StrongWorkflowId(std::int64_t value) : value_(value) {}

    [[nodiscard]] constexpr std::int64_t value() const { return value_; }
    [[nodiscard]] constexpr bool valid() const { return value_ > 0; }

    auto operator<=>(const StrongWorkflowId&) const = default;

private:
    std::int64_t value_ = 0;
};

struct WorkflowInstanceIdTag;
struct WorkflowExpansionIdTag;
struct WorkflowExpansionMemberIdTag;

using WorkflowInstanceId = StrongWorkflowId<WorkflowInstanceIdTag>;
using WorkflowExpansionId = StrongWorkflowId<WorkflowExpansionIdTag>;
using WorkflowExpansionMemberId = StrongWorkflowId<WorkflowExpansionMemberIdTag>;

struct ExpansionHeaderPresentationKey {
    WorkflowExpansionId expansion_id;
    auto operator<=>(const ExpansionHeaderPresentationKey&) const = default;
};

struct ExpansionMemberPresentationKey {
    WorkflowExpansionMemberId member_id;
    auto operator<=>(const ExpansionMemberPresentationKey&) const = default;
};

struct StandaloneWorkflowPresentationKey {
    WorkflowInstanceId workflow_id;
    auto operator<=>(const StandaloneWorkflowPresentationKey&) const = default;
};

using WorkflowPresentationKey = std::variant<
    ExpansionHeaderPresentationKey,
    ExpansionMemberPresentationKey,
    StandaloneWorkflowPresentationKey>;

} // namespace savor::db::execution::workflow
