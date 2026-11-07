#include <gtest/gtest.h>

#include <stdexcept>

#include "ruvia/core/task.h"

#include "streaming_fixture.h"

// Scoped operations release cold frames and shared registrations exactly once.

TEST(Ruvia, scoped_operation_destruction_destroys_cold_frame) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    {
        auto operation =
            ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
        EXPECT_TRUE(!destroyed);
        (void)operation;
    }
    EXPECT_TRUE(destroyed);
}

TEST(Ruvia, scoped_capability_move_relinks_and_parent_close_expires_destination) {
    ruvia::operation_scope scope;
    int expired_count = 0;
    test_scoped_capability first(scope, expired_count);
    test_scoped_capability moved(std::move(first));
    moved.use();
    scope.close();
    EXPECT_EQ(expired_count, 1);
    bool rejected = false;
    try {
        moved.use();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, scoped_capability_copy_relinks_and_early_destruction_unlinks) {
    int expired_count = 0;
    ruvia::operation_scope scope;
    test_scoped_capability source(scope, expired_count);
    {
        test_scoped_capability destroyed_early(source);
        destroyed_early.use();
    }
    test_scoped_capability surviving_copy(source);
    scope.close();
    EXPECT_EQ(expired_count, 2);

    bool source_rejected = false;
    try {
        source.use();
    } catch (const std::logic_error&) {
        source_rejected = true;
    }
    EXPECT_TRUE(source_rejected);
}

TEST(Ruvia, scoped_operation_parent_close_destroys_cold_frame_immediately) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    auto operation =
        ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
    EXPECT_TRUE(!destroyed);
    scope.close();
    EXPECT_TRUE(destroyed);
    (void)operation;
}
