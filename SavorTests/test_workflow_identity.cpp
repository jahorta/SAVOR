#include "Execution/Workflow/WorkflowIdentity.h"

#include <gtest/gtest.h>

#include <set>

namespace workflow = savor::db::execution::workflow;

TEST(WorkflowIdentity, NumericValuesDoNotCollideAcrossPresentationDomains)
{
    const workflow::WorkflowPresentationKey header =
        workflow::ExpansionHeaderPresentationKey{workflow::WorkflowExpansionId{7}};
    const workflow::WorkflowPresentationKey member =
        workflow::ExpansionMemberPresentationKey{workflow::WorkflowExpansionMemberId{7}};
    const workflow::WorkflowPresentationKey standalone =
        workflow::StandaloneWorkflowPresentationKey{workflow::WorkflowInstanceId{7}};

    EXPECT_NE(header, member);
    EXPECT_NE(header, standalone);
    EXPECT_NE(member, standalone);
    EXPECT_EQ((std::set<workflow::WorkflowPresentationKey>{header, member, standalone}).size(), 3u);
}

TEST(WorkflowIdentity, SharedWorkflowMembershipsRemainDistinctOccurrences)
{
    const workflow::WorkflowInstanceId sharedWorkflow{41};
    const workflow::WorkflowPresentationKey firstOccurrence =
        workflow::ExpansionMemberPresentationKey{workflow::WorkflowExpansionMemberId{101}};
    const workflow::WorkflowPresentationKey secondOccurrence =
        workflow::ExpansionMemberPresentationKey{workflow::WorkflowExpansionMemberId{102}};

    EXPECT_TRUE(sharedWorkflow.valid());
    EXPECT_NE(firstOccurrence, secondOccurrence);
}
