#include <atomic>
#include <barrier>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/web/app.h"
#include "ruvia/web/web_worker.h"

#include "app/web_worker_dispatch.h"
#include "integration/worker_capabilities.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

struct worker_dispatch_fixture final {
    asio::io_context& io_context_ = ruvia::test::new_test_io_context();
    ruvia::event_loop_attachment attachment_ =
        ruvia::attach_event_loop(io_context_, {.queue_capacity_ = 1});
    ruvia::worker_handle worker_ = attachment_.loop().handle();
    ruvia::worker_memory memory_;
    ruvia::detail::worker_capabilities capabilities_{
        io_context_, worker_, memory_.resource(), {}, {}};
    std::shared_ptr<ruvia::detail::web_worker_dispatch> dispatch_;

    worker_dispatch_fixture()
        : dispatch_(std::make_shared<ruvia::detail::web_worker_dispatch>(io_context_.get_executor(),
              worker_, memory_.resource(), capabilities_,
              [](std::exception_ptr) noexcept {})) {
        capabilities_.initialize_worker_state();
    }

    void retire() {
        attachment_.stop();
        // External attachments detach on their io_context; drain that terminal
        // cleanup before checking reservations held by abandoned queue posts.
        io_context_.poll();
        dispatch_->retire();
        capabilities_.close_now();
        capabilities_.shutdown_worker_state();
    }
};

ruvia::task<void> empty_task(ruvia::web_worker_context&) {
    co_return;
}

}  // namespace

RUVIA_TEST(app_server_rejects_zero_http_client_result_budget) {
    ruvia::server_config config;
    config.http_client_result_budget_.max_retained_bytes_ = 0;

    bool rejected = false;
    try {
        ruvia::app().server(config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(web_worker_context_pool_is_worker_resource) {
    worker_dispatch_fixture fixture;
    std::pmr::memory_resource* observed_value = nullptr;

    const auto result_value = fixture.dispatch_->handle().post([&observed_value](ruvia::web_worker_context& context_value) {
        observed_value = context_value.pool();
        return empty_task(context_value);
    });
    RUVIA_CHECK(result_value.accepted());

    fixture.io_context_.poll();

    RUVIA_CHECK(observed_value == fixture.memory_.resource());
    fixture.retire();
}

RUVIA_TEST(web_worker_dispatch_completes_started_task_and_releases_reservation) {
    worker_dispatch_fixture fixture;
    std::atomic_bool ran{false};

    const auto result_value = fixture.dispatch_->handle().post([&ran](ruvia::web_worker_context& context_value) {
        ran.store(true, std::memory_order_release);
        return empty_task(context_value);
    });
    RUVIA_CHECK(result_value.accepted());

    fixture.io_context_.poll();

    const auto stats = fixture.dispatch_->stats();
    RUVIA_CHECK(ran.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(stats.completed_, 1U);
    RUVIA_CHECK_EQ(stats.outstanding_, 0U);
    fixture.retire();
}

RUVIA_TEST(web_worker_dispatch_reconciles_rejected_and_abandoned_posts) {
    worker_dispatch_fixture fixture;
    bool ran = false;

    // Run the terminal stop on the worker before its queued queue drain.
    // Stopping from outside poll() defers detach behind that drain instead.
    asio::post(fixture.io_context_, [&fixture] { fixture.attachment_.stop(); });
    const auto accepted = fixture.dispatch_->handle().post([&ran](ruvia::web_worker_context& context_value) {
        ran = true;
        return empty_task(context_value);
    });
    RUVIA_CHECK(accepted.accepted());
    auto full = fixture.dispatch_->handle().post(empty_task);
    RUVIA_CHECK_EQ(full.status(), ruvia::post_status::queue_full);
    RUVIA_CHECK(full.rejected() != nullptr);

    fixture.dispatch_->close();

    auto stopped = fixture.dispatch_->handle().post(empty_task);
    RUVIA_CHECK_EQ(stopped.status(), ruvia::post_status::worker_stopping);
    RUVIA_CHECK(stopped.rejected() != nullptr);

    fixture.io_context_.poll();
    fixture.retire();
    RUVIA_CHECK(!ran);
    RUVIA_CHECK_EQ(fixture.dispatch_->stats().completed_, 0U);
    RUVIA_CHECK_EQ(fixture.dispatch_->stats().outstanding_, 0U);
}

RUVIA_TEST(web_worker_dispatch_retires_late_factory_producer) {
    worker_dispatch_fixture fixture;
    struct state_type final {
        std::barrier<> producer_ready_{2};
        std::binary_semaphore factory_entered_{0};
        std::binary_semaphore release_factory_{0};
        ruvia::detail::web_worker_dispatch* dispatch_{nullptr};
        std::atomic_bool paused_{false};
        std::atomic_bool ran_{false};
        std::atomic_uint32_t live_destructions_{0};
    } state_value;
    state_value.dispatch_ = fixture.dispatch_.get();

    struct producer final {
        state_type* state_;
        bool live_ = true;

        explicit producer(state_type& value) noexcept
            : state_(&value) {}

        producer(const producer&) = delete;
        producer& operator=(const producer&) = delete;

        producer(producer&& other) noexcept
            : state_(other.state_),
              live_(std::exchange(other.live_, false)) {
            if (state_->dispatch_->stats().outstanding_ != 0 &&
                !state_->paused_.exchange(true, std::memory_order_acq_rel)) {
                state_->factory_entered_.release();
                state_->release_factory_.acquire();
            }
        }

        ~producer() {
            if (live_) {
                state_->live_destructions_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        ruvia::task<void> operator()(ruvia::web_worker_context&) {
            state_->ran_.store(true, std::memory_order_release);
            co_return;
        }
    };

    std::atomic<ruvia::post_status> status{ruvia::post_status::worker_stopping};
    std::thread producer_value([&] {
        state_value.producer_ready_.arrive_and_wait();
        const auto result_value = fixture.dispatch_->handle().post(producer{state_value});
        status.store(result_value.status(), std::memory_order_release);
    });
    state_value.producer_ready_.arrive_and_wait();
    state_value.factory_entered_.acquire();

    fixture.retire();
    state_value.release_factory_.release();
    producer_value.join();
    // Retirement retains the context until reserved publication quiesces.
    // Drive the late factory's abandonment before destroying its owner.
    fixture.io_context_.poll();

    const auto stats = fixture.dispatch_->stats();
    RUVIA_CHECK_EQ(status.load(std::memory_order_acquire), ruvia::post_status::accepted);
    RUVIA_CHECK(!state_value.ran_.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(state_value.live_destructions_.load(std::memory_order_acquire), 1U);
    RUVIA_CHECK_EQ(stats.completed_, 0U);
    RUVIA_CHECK_EQ(stats.outstanding_, 0U);
}
