#include <gtest/gtest.h>

#include "Worker/TSQueue.h"

TEST(TSQueue, RejectsPushAfterCloseAndUnblocksWaiter) {
    TSQueue<int> q;

    q.close();

    EXPECT_FALSE(q.push(1));

    int out = 0;
    EXPECT_FALSE(q.pop_wait(out));

    q.reset();
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.pop_wait(out));
    EXPECT_EQ(2, out);
}

TEST(TSQueue, DrainsQueuedItemsAfterCloseThenStops) {
    TSQueue<int> q;

    EXPECT_TRUE(q.push(7));
    q.close();

    int out = 0;
    EXPECT_TRUE(q.pop_wait(out));
    EXPECT_EQ(7, out);
    EXPECT_FALSE(q.pop_wait(out));
}
