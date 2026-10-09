#include <stdexcept>

#include "ruvia/core/task.h"

#include "streaming_fixture.h"

// Scoped operations release cold frames and shared registrations exactly once.

RUVIA_TEST(scoped_operation_destruction_destroys_cold_frame) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    {
        auto operation =
            ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
        RUVIA_CHECK(!destroyed);
        (void)operation;
    }
    RUVIA_CHECK(destroyed);
}

RUVIA_TEST(scoped_capability_move_relinks_and_parent_close_expires_destination) {
    ruvia::operation_scope scope;
    int expired_count = 0;
    test_scoped_capability first(scope, expired_count);
    test_scoped_capability moved(std::move(first));
    moved.use();
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 1);
    bool rejected = false;
    try {
        moved.use();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(scoped_capability_copy_relinks_and_early_destruction_unlinks) {
    int expired_count = 0;
    ruvia::operation_scope scope;
    test_scoped_capability source(scope, expired_count);
    {
        test_scoped_capability destroyed_early(source);
        destroyed_early.use();
    }
    test_scoped_capability surviving_copy(source);
    scope.close();
    RUVIA_CHECK_EQ(expired_count, 2);

    bool source_rejected = false;
    try {
        source.use();
    } catch (const std::logic_error&) {
        source_rejected = true;
    }
    RUVIA_CHECK(source_rejected);
}

RUVIA_TEST(scoped_operation_parent_close_destroys_cold_frame_immediately) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    auto operation =
        ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
    RUVIA_CHECK(!destroyed);
    scope.close();
    RUVIA_CHECK(destroyed);
    (void)operation;
}
