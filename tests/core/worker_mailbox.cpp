#include <initializer_list>
#include <memory>

#include <asio/io_context.hpp>

#include "ruvia/core/detail/worker/WorkerDispatcher.h"

#include "test_harness.h"

namespace {

struct release_observation final {
    bool destroyed_{false};
    bool reentered_{false};
    bool idle_after_destruction_{false};
    bool ran_{false};
};

struct reentrant_payload final {
    ruvia::detail::WorkerDispatcher& dispatcher_;
    release_observation& observation_;

    ~reentrant_payload() noexcept {
        observation_.destroyed_ = true;
        // Outside a running owner this query acquires the dispatcher mutex,
        // proving payload destruction is not occurring under that mutex.
        observation_.reentered_ = !dispatcher_.isCurrent();
    }
};

ruvia::MoveOnlyFunction<void()> make_payload(
    ruvia::detail::WorkerDispatcher& dispatcher, release_observation& observation) {
    dispatcher.whenIdle([&observation] {
        observation.idle_after_destruction_ = observation.destroyed_ && observation.reentered_;
    });
    auto payload = std::make_unique<reentrant_payload>(dispatcher, observation);
    return [payload = std::move(payload), &observation] { observation.ran_ = true; };
}

}  // namespace

RUVIA_TEST(worker_mailbox_abandoned_payload_reenters_before_idle_completion) {
    // Queued work and an unpublished factory result use the same retirement
    // sequence, despite claiming their releasing nodes at different boundaries.
    for (const bool detach_in_factory : {false, true}) {
        asio::io_context context;
        auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(context, 1);
        release_observation observation;
        const auto status = dispatcher->postFactory([&] {
            auto payload = make_payload(*dispatcher, observation);
            if (detach_in_factory) {
                dispatcher->detachContext();
            }
            return payload;
        });
        RUVIA_CHECK(status == ruvia::PostStatus::kAccepted);
        if (!detach_in_factory) {
            dispatcher->detachContext();
        }
        dispatcher->waitForReservations();
        context.run();
        RUVIA_CHECK(!observation.ran_);
        RUVIA_CHECK(observation.destroyed_);
        RUVIA_CHECK(observation.reentered_);
        RUVIA_CHECK(observation.idle_after_destruction_);
    }
}

RUVIA_TEST(worker_mailbox_failed_publication_recovers_capacity_and_destroys_payload_before_idle) {
    asio::io_context context;
    // Without a shared owner publication cannot acquire the drain's lifetime.
    // Repeated attempts must recover the single reservation rather than fill it.
    ruvia::detail::WorkerDispatcher dispatcher(context, 1);
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        release_observation observation;
        bool failed = false;
        try {
            (void)dispatcher.postFactory([&] { return make_payload(dispatcher, observation); });
        } catch (const std::bad_weak_ptr&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        dispatcher.waitForReservations();
        RUVIA_CHECK(!observation.ran_);
        RUVIA_CHECK(observation.destroyed_);
        RUVIA_CHECK(observation.reentered_);
        RUVIA_CHECK(observation.idle_after_destruction_);
    }
    dispatcher.detachContext();
}
