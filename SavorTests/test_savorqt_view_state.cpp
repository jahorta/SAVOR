#include <gtest/gtest.h>

#include "../SavorQt/GUI/Refresh/ViewState.h"

#include <string>

namespace {

using savorqt::gui::AppliedQuery;
using savorqt::gui::DraftReplacementReason;
using savorqt::gui::DraftState;
using savorqt::gui::EditorSession;
using savorqt::gui::KeyedSelection;

TEST(SavorQtViewState, DirtyDraftSurvivesBackingRefresh)
{
    DraftState<std::string> draft;
    draft.replace("saved", DraftReplacementReason::InitialLoad);
    draft.edit("user edit");
    draft.observeBacking("new saved value");

    EXPECT_TRUE(draft.dirty());
    EXPECT_TRUE(draft.backingStale());
    EXPECT_EQ(draft.value(), "user edit");
    EXPECT_EQ(draft.baseline(), "saved");
}

TEST(SavorQtViewState, ExplicitReplacementAndSuccessfulSaveResetBaseline)
{
    DraftState<std::string> draft;
    draft.replace("initial", DraftReplacementReason::InitialLoad);
    draft.edit("reset value");
    draft.replace("reset value", DraftReplacementReason::ExplicitReset);
    EXPECT_FALSE(draft.dirty());

    draft.edit("saved value");
    draft.commit();
    EXPECT_FALSE(draft.dirty());
    EXPECT_EQ(draft.baseline(), "saved value");
}

TEST(SavorQtViewState, AppliedQueryDoesNotChangeWithDraft)
{
    DraftState<std::string> draft;
    AppliedQuery<std::string> query;
    draft.replace("alpha", DraftReplacementReason::InitialLoad);
    query.apply(draft.value());
    draft.edit("beta");

    EXPECT_EQ(query.value(), "alpha");
    EXPECT_EQ(draft.value(), "beta");
}

TEST(SavorQtViewState, KeyedSelectionDoesNotInventFallback)
{
    KeyedSelection<long long> selection;
    selection.select(42);
    EXPECT_TRUE(selection.is(42));
    selection.clear();
    EXPECT_FALSE(selection.key().has_value());
}

TEST(SavorQtViewState, EditorSessionPreservesDirtyDraftWhenBackingDisappears)
{
    EditorSession<long long, std::string> session;
    session.active = true;
    session.entity.bind(7, "saved", DraftReplacementReason::UserRequestedEntityChange);
    session.entity.draft.edit("unsaved");
    session.entity.draft.markBackingMissing();

    EXPECT_TRUE(session.active);
    EXPECT_TRUE(session.entity.draft.dirty());
    EXPECT_TRUE(session.entity.draft.backingMissing());
    EXPECT_EQ(session.entity.draft.value(), "unsaved");
}

} // namespace
