#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_config_storage.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"
#include "client/http_client_tunnel_state.h"
#include "client/http_client_upload_state.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class allocation_counter final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_allocations_;
    }
    [[nodiscard]] std::size_t live_bytes() const noexcept {
        return live_bytes_;
    }
    [[nodiscard]] std::size_t large_allocations() const noexcept {
        return large_allocations_;
    }
    [[nodiscard]] std::size_t large_bytes() const noexcept {
        return large_bytes_;
    }
    [[nodiscard]] std::size_t small_cached_allocations() const noexcept {
        return small_cached_allocations_;
    }
    [[nodiscard]] std::size_t small_cached_bytes() const noexcept {
        return small_cached_bytes_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        void* const result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++live_allocations_;
        live_bytes_ += bytes_value;
        if (bytes_value >= large_allocation_bytes) {
            ++large_allocations_;
            large_bytes_ += bytes_value;
        } else {
            ++small_cached_allocations_;
            small_cached_bytes_ += bytes_value;
        }
        return result_value;
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        --live_allocations_;
        live_bytes_ -= bytes_value;
        if (bytes_value >= large_allocation_bytes) {
            --large_allocations_;
            large_bytes_ -= bytes_value;
        } else {
            --small_cached_allocations_;
            small_cached_bytes_ -= bytes_value;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    static constexpr std::size_t large_allocation_bytes = 256 * 1024;
    std::size_t live_allocations_{};
    std::size_t live_bytes_{};
    std::size_t large_allocations_{};
    std::size_t large_bytes_{};
    std::size_t small_cached_allocations_{};
    std::size_t small_cached_bytes_{};
};

class test_worker final {
public:
    explicit test_worker(asio::io_context& io)
        : attachment_(ruvia::attach_event_loop(io, {.queue_capacity_ = 8})),
          handle_(attachment_.loop().handle()) {}

    ruvia::event_loop_attachment attachment_;
    ruvia::worker_handle handle_;
};

template <typename operation_type>
void run_operation(test_worker& worker_value, asio::io_context& io, operation_type&& operation) {
    auto run = [&]() -> ruvia::task<void> {
        co_await operation();
        worker_value.attachment_.stop();
    };
    auto root = worker_value.attachment_.loop().start(run());
    worker_value.attachment_.run();
    root.get();
    io.restart();
}

ruvia::task<void> await_cancelled_response(ruvia::detail::http_client_response_state& state_value,
    bool& caught_expected_error) {
    try {
        (void)co_await ruvia::make_scoped_operation(
            state_value.body_operation_scope_, state_value.read_all(2 * 1024 * 1024));
    } catch (const ruvia::http_client_error& error) {
        caught_expected_error = error.code() == ruvia::http_client_error::code_type::cancelled;
    }
}

ruvia::task<void> await_failed_response(ruvia::detail::http_client_response_state& state_value,
    bool& caught_expected_error) {
    try {
        (void)co_await ruvia::make_scoped_operation(
            state_value.body_operation_scope_, state_value.read_all(2 * 1024 * 1024));
    } catch (const std::runtime_error& error) {
        caught_expected_error = std::string_view(error.what()) == "response fixture failed";
    }
}

ruvia::task<void> await_signal(ruvia::task<void> wait, bool& woke) {
    co_await std::move(wait);
    woke = true;
}

void count_queue_wake(void* target) noexcept {
    ++*static_cast<std::size_t*>(target);
}

}  // namespace

RUVIA_TEST(client_response_memory_output_queues_stop_once_and_retire_transport_wakes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    allocation_counter upstream;

    run_operation(worker, io, [&]() -> ruvia::task<void> {
        auto budget = std::make_shared<ruvia::detail::http_client_result_budget_domain>(
            ruvia::http_client_result_budget_config{.max_retained_bytes_ = 1024 * 1024});
        ruvia::detail::http_client_pool pool(io, worker.handle_,
            ruvia::detail::http_client_config_storage(ruvia::http_client_config{.host_ = "unused.test"},
                std::pmr::get_default_resource()),
            ruvia::http_client_result_budget_config{.max_retained_bytes_ = 1024 * 1024},
            std::pmr::get_default_resource());
        auto domain = ruvia::detail::http_client_response_memory_domain::create(
            worker.handle_, budget, upstream);
        auto* const state_value = domain->create_state(pool);
        state_value->tunnel_.emplace(domain->worker(), state_value->resource_, ruvia::http_client_tunnel_config{});
        state_value->upload_.emplace(domain->worker(), state_value->resource_, ruvia::http_client_upload_config{});

        std::size_t tunnel_wake_count{};
        std::size_t upload_wake_count{};
        state_value->tunnel_->output_.wake_target_ = &tunnel_wake_count;
        state_value->tunnel_->output_.wake_ = count_queue_wake;
        state_value->upload_->output_.wake_target_ = &upload_wake_count;
        state_value->upload_->output_.wake_ = count_queue_wake;
        const std::string tunnel_payload(512, 't');
        const std::string upload_payload(512, 'u');
        state_value->tunnel_->output_.chunk_.assign(tunnel_payload);
        state_value->tunnel_->output_.chunk_ready_ = true;
        state_value->upload_->output_.chunk_.assign(upload_payload);
        state_value->upload_->output_.chunk_ready_ = true;
        const auto live_allocations = upstream.live_allocations();
        const auto live_bytes = upstream.live_bytes();
        RUVIA_CHECK(live_allocations > 0);
        RUVIA_CHECK(live_bytes > 0);

        bool tunnel_data_woke = false;
        bool tunnel_space_woke = false;
        bool upload_data_woke = false;
        bool upload_space_woke = false;
        ruvia::task_scope waiters(worker.handle_);
        waiters.spawn(await_signal(state_value->tunnel_->output_.data_.wait(), tunnel_data_woke));
        waiters.spawn(await_signal(state_value->tunnel_->output_.space_.wait(), tunnel_space_woke));
        waiters.spawn(await_signal(state_value->upload_->output_.data_.wait(), upload_data_woke));
        waiters.spawn(await_signal(state_value->upload_->output_.space_.wait(), upload_space_woke));

        state_value->tunnel_->output_.stop();
        state_value->upload_->output_.stop();
        co_await waiters.join();
        RUVIA_CHECK(tunnel_data_woke);
        RUVIA_CHECK(tunnel_space_woke);
        RUVIA_CHECK(upload_data_woke);
        RUVIA_CHECK(upload_space_woke);
        RUVIA_CHECK_EQ(tunnel_wake_count, std::size_t{1});
        RUVIA_CHECK_EQ(upload_wake_count, std::size_t{1});
        RUVIA_CHECK_EQ(upstream.live_allocations(), live_allocations);
        RUVIA_CHECK_EQ(upstream.live_bytes(), live_bytes);

        // Repeated stop is a no-op for external wake registrations, and the
        // queued span remains alive until its consumer explicitly releases it.
        state_value->tunnel_->output_.stop();
        state_value->upload_->output_.stop();
        RUVIA_CHECK_EQ(tunnel_wake_count, std::size_t{1});
        RUVIA_CHECK_EQ(upload_wake_count, std::size_t{1});
        RUVIA_CHECK_EQ(std::string_view(state_value->tunnel_->output_.chunk_), tunnel_payload);
        RUVIA_CHECK_EQ(std::string_view(state_value->upload_->output_.chunk_), upload_payload);
        RUVIA_CHECK_EQ(upstream.live_allocations(), live_allocations);
        RUVIA_CHECK_EQ(upstream.live_bytes(), live_bytes);

        domain->detach_transport_bindings(pool);
        RUVIA_CHECK(state_value->tunnel_->output_.wake_ == nullptr);
        RUVIA_CHECK(state_value->tunnel_->output_.wake_target_ == nullptr);
        RUVIA_CHECK(state_value->upload_->output_.wake_ == nullptr);
        RUVIA_CHECK(state_value->upload_->output_.wake_target_ == nullptr);
        RUVIA_CHECK_EQ(tunnel_wake_count, std::size_t{1});
        RUVIA_CHECK_EQ(upload_wake_count, std::size_t{1});
        RUVIA_CHECK_EQ(std::string_view(state_value->tunnel_->output_.chunk_), tunnel_payload);
        RUVIA_CHECK_EQ(std::string_view(state_value->upload_->output_.chunk_), upload_payload);
        RUVIA_CHECK_EQ(upstream.live_allocations(), live_allocations);
        RUVIA_CHECK_EQ(upstream.live_bytes(), live_bytes);

        state_value->release_reference();
        domain.reset();
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.live_bytes(), std::size_t{0});
        pool.close_now();
        co_await pool.join();
    });
}

RUVIA_TEST(client_response_memory_result_releases_after_client_worker_owner_dies) {
    auto& io = ruvia::test::new_test_io_context();
    allocation_counter upstream;
    auto budget = std::make_shared<ruvia::detail::http_client_result_budget_domain>(
        ruvia::http_client_result_budget_config{.max_retained_bytes_ = 1024});
    std::optional<ruvia::http_client_response_bytes> held;
    const std::string body(512, 'r');

    {
        test_worker worker(io);
        run_operation(worker, io, [&]() -> ruvia::task<void> {
            ruvia::detail::http_client_pool pool(io, worker.handle_,
                ruvia::detail::http_client_config_storage(ruvia::http_client_config{.host_ = "unused.test"},
                    std::pmr::get_default_resource()),
                ruvia::http_client_result_budget_config{.max_retained_bytes_ = 1024},
                std::pmr::get_default_resource());
            auto domain = ruvia::detail::http_client_response_memory_domain::create(
                worker.handle_, budget, upstream);
            auto* const state_value = domain->create_state(pool);
            state_value->buffered_.assign(body);
            state_value->complete_ = true;

            held.emplace(co_await ruvia::make_scoped_operation(
                state_value->body_operation_scope_, state_value->read_all(body.size())));
            RUVIA_CHECK_EQ(held->size(), body.size());
            RUVIA_CHECK_EQ(budget->retained_bytes(), body.size());

            domain->detach_transport_bindings(pool);
            state_value->release_reference();
            domain.reset();
            pool.close_now();
            co_await pool.join();
        });
    }

    RUVIA_CHECK(held.has_value());
    RUVIA_CHECK_EQ(held->bytes().size(), body.size());
    RUVIA_CHECK_EQ(held->bytes().front(), std::byte{'r'});
    RUVIA_CHECK_EQ(budget->retained_bytes(), body.size());
    held.reset();
    RUVIA_CHECK_EQ(budget->retained_bytes(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.live_bytes(), std::size_t{0});
}

RUVIA_TEST(client_response_memory_domain_pins_state_and_releases_pooled_storage_last) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    allocation_counter upstream;
    std::weak_ptr<ruvia::detail::http_client_result_budget_domain> budget_lifetime;
    std::optional<ruvia::http_client_response_bytes> retained;

    run_operation(worker, io, [&]() -> ruvia::task<void> {
        auto budget = std::make_shared<ruvia::detail::http_client_result_budget_domain>(
            ruvia::http_client_result_budget_config{.max_retained_bytes_ = 2 * 1024 * 1024 + 8});
        budget_lifetime = budget;
        ruvia::detail::http_client_pool pool(io, worker.handle_,
            ruvia::detail::http_client_config_storage(ruvia::http_client_config{.host_ = "unused.test"},
                std::pmr::get_default_resource()),
            ruvia::http_client_result_budget_config{.max_retained_bytes_ = 3 * 1024 * 1024},
            std::pmr::get_default_resource());
        auto domain = ruvia::detail::http_client_response_memory_domain::create(
            worker.handle_, budget, upstream);
        budget.reset();
        auto* const state_value = domain->create_state(pool);

        const std::string header_value(512 * 1024, 'h');
        const std::string trailer_value(512 * 1024, 't');
        const std::string body(1024 * 1024, 'b');
        state_value->headers_.push_back(ruvia::http_header::copy_of("x-long", header_value, state_value->resource_));
        state_value->trailers_.push_back(ruvia::http_header::copy_of("x-trailer", trailer_value, state_value->resource_));
        state_value->buffered_.assign(body);
        state_value->pending_.assign("tail");
        state_value->complete_ = true;

        domain->detach_transport_bindings(pool);
        RUVIA_CHECK(state_value->pool_ == nullptr);
        RUVIA_CHECK_EQ(state_value->headers_.front().value(), std::string_view(header_value));
        RUVIA_CHECK_EQ(state_value->trailers_.front().value(), std::string_view(trailer_value));
        RUVIA_CHECK_EQ(std::string_view(state_value->buffered_), body);
        RUVIA_CHECK_EQ(std::string_view(state_value->pending_), "tail");
        RUVIA_CHECK(upstream.large_bytes() >= header_value.size() + trailer_value.size());

        std::size_t cached_large_bytes{};
        std::size_t cached_small_bytes{};
        auto operation = [&]() -> ruvia::task<void> {
            std::optional<ruvia::http_client_response_bytes> budget_filler;
            {
                auto cold = ruvia::make_scoped_operation(
                    state_value->body_operation_scope_, state_value->read_all(body.size() + 4));
            }
            RUVIA_CHECK_EQ(std::string_view(state_value->buffered_), body);
            RUVIA_CHECK_EQ(std::string_view(state_value->pending_), "tail");

            bool size_rejected = false;
            try {
                (void)co_await ruvia::make_scoped_operation(
                    state_value->body_operation_scope_, state_value->read_all(1));
            } catch (const ruvia::http_client_error& error) {
                size_rejected = error.code() == ruvia::http_client_error::code_type::response_too_large;
            }
            RUVIA_CHECK(size_rejected);
            RUVIA_CHECK_EQ(std::string_view(state_value->buffered_), body);
            RUVIA_CHECK_EQ(std::string_view(state_value->pending_), "tail");

            for (int iteration = 0; iteration < 16; ++iteration) {
                if (iteration != 0) {
                    state_value->buffered_.assign(body);
                    state_value->pending_.assign("tail");
                    state_value->offset_ = 0;
                }
                auto result_value = co_await ruvia::make_scoped_operation(
                    state_value->body_operation_scope_, state_value->read_all(body.size() + 4));
                RUVIA_CHECK_EQ(result_value.size(), body.size() + 4);
                RUVIA_CHECK_EQ(result_value.bytes().front(), std::byte{'b'});
                RUVIA_CHECK_EQ(result_value.bytes().back(), std::byte{'l'});
                RUVIA_CHECK_EQ(state_value->headers_.front().value(), std::string_view(header_value));
                RUVIA_CHECK_EQ(state_value->trailers_.front().value(), std::string_view(trailer_value));
                if (iteration == 0) {
                    retained.emplace(std::move(result_value));
                }
                RUVIA_CHECK_EQ(retained->bytes().front(), std::byte{'b'});
                RUVIA_CHECK_EQ(retained->bytes().back(), std::byte{'l'});
                if (iteration == 0) {
                    RUVIA_CHECK_EQ(budget_lifetime.lock()->retained_bytes(), body.size() + 4);
                    state_value->buffered_.assign(body);
                    state_value->pending_.assign("tail");
                    auto second = co_await ruvia::make_scoped_operation(
                        state_value->body_operation_scope_, state_value->read_all(body.size() + 4));
                    budget_filler.emplace(std::move(second));

                    state_value->buffered_.assign(body);
                    state_value->pending_.assign("tail");
                    bool budget_full = false;
                    try {
                        (void)co_await ruvia::make_scoped_operation(
                            state_value->body_operation_scope_, state_value->read_all(body.size() + 4));
                    } catch (const ruvia::http_client_error& error) {
                        budget_full = error.code() ==
                                      ruvia::http_client_error::code_type::result_budget_exceeded;
                    }
                    RUVIA_CHECK(budget_full);
                    RUVIA_CHECK_EQ(std::string_view(state_value->buffered_), body);
                    RUVIA_CHECK_EQ(std::string_view(state_value->pending_), "tail");
                    budget_filler.reset();
                    auto retry = co_await ruvia::make_scoped_operation(
                        state_value->body_operation_scope_, state_value->read_all(body.size() + 4));
                    RUVIA_CHECK_EQ(retry.size(), body.size() + 4);
                } else {
                    RUVIA_CHECK_EQ(upstream.large_bytes(), cached_large_bytes);
                    RUVIA_CHECK_EQ(upstream.small_cached_bytes(), cached_small_bytes);
                }
                if (iteration == 0) {
                    cached_large_bytes = upstream.large_bytes();
                    cached_small_bytes = upstream.small_cached_bytes();
                }
            }
            state_value->body_operation_scope_.close();
            co_await state_value->body_operation_scope_.close_and_join();
            bool expired_scope_rejected = false;
            try {
                auto expired = ruvia::make_scoped_operation(
                    state_value->body_operation_scope_, state_value->read_all(body.size()));
                (void)co_await std::move(expired);
            } catch (const std::logic_error&) {
                expired_scope_rejected = true;
            }
            RUVIA_CHECK(expired_scope_rejected);
        };
        co_await operation();

        domain.reset();
        RUVIA_CHECK(!budget_lifetime.expired());
        RUVIA_CHECK(worker.handle_.is_current());
        RUVIA_CHECK(state_value->resource_ != nullptr);
        if (auto live_budget = budget_lifetime.lock()) {
            RUVIA_CHECK_EQ(live_budget->retained_bytes(), std::size_t{1024 * 1024 + 4});
        } else {
            RUVIA_CHECK(false);
        }
        RUVIA_CHECK(upstream.live_allocations() > 0);
        state_value->release_reference();
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.live_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.large_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.large_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.small_cached_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.small_cached_bytes(), std::size_t{0});
        RUVIA_CHECK(!budget_lifetime.expired());

        pool.close_now();
        co_await pool.join();
    });
    RUVIA_CHECK(retained.has_value());
    RUVIA_CHECK_EQ(retained->bytes().size(), std::size_t{1024 * 1024 + 4});
    retained.reset();
    RUVIA_CHECK(budget_lifetime.expired());
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
}

RUVIA_TEST(client_response_memory_domain_joins_error_waiters_before_releasing_storage) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    allocation_counter upstream;
    std::weak_ptr<ruvia::detail::http_client_result_budget_domain> budget_lifetime;

    run_operation(worker, io, [&]() -> ruvia::task<void> {
        auto budget = std::make_shared<ruvia::detail::http_client_result_budget_domain>(
            ruvia::http_client_result_budget_config{.max_retained_bytes_ = 2 * 1024 * 1024});
        budget_lifetime = budget;
        ruvia::detail::http_client_pool pool(io, worker.handle_,
            ruvia::detail::http_client_config_storage(ruvia::http_client_config{.host_ = "unused.test"},
                std::pmr::get_default_resource()),
            ruvia::http_client_result_budget_config{.max_retained_bytes_ = 2 * 1024 * 1024},
            std::pmr::get_default_resource());
        auto domain = ruvia::detail::http_client_response_memory_domain::create(
            worker.handle_, budget, upstream);
        budget.reset();

        auto* const cancelled_state = domain->create_state(pool);
        auto* const failed_state = domain->create_state(pool);
        const std::string body(1024 * 1024, 'c');
        const std::string header_value(512 * 1024, 'h');
        for (auto* state : {cancelled_state, failed_state}) {
            state->headers_.push_back(ruvia::http_header::copy_of("x-retained", header_value, state->resource_));
            state->buffered_.assign(body);
        }
        domain.reset();
        RUVIA_CHECK(!budget_lifetime.expired());
        RUVIA_CHECK(upstream.large_bytes() >= 2 * (body.size() + header_value.size()));

        bool cancelled_as_expected = false;
        ruvia::task_scope cancelled_tasks(worker.handle_);
        cancelled_tasks.spawn(await_cancelled_response(*cancelled_state, cancelled_as_expected));
        RUVIA_CHECK(cancelled_state->collect_all_);
        cancelled_state->error_code_ =
            static_cast<std::uint8_t>(ruvia::http_client_error::code_type::cancelled);
        cancelled_state->complete_ = true;
        cancelled_state->data_signal_.notify();
        co_await cancelled_tasks.join();
        cancelled_state->body_operation_scope_.close();
        co_await cancelled_state->body_operation_scope_.close_and_join();
        RUVIA_CHECK(cancelled_as_expected);
        RUVIA_CHECK_EQ(std::string_view(cancelled_state->buffered_), body);
        RUVIA_CHECK_EQ(cancelled_state->headers_.front().value(), std::string_view(header_value));

        bool failure_as_expected = false;
        ruvia::task_scope failed_tasks(worker.handle_);
        failed_tasks.spawn(await_failed_response(*failed_state, failure_as_expected));
        RUVIA_CHECK(failed_state->collect_all_);
        failed_state->failure_ = std::make_exception_ptr(std::runtime_error("response fixture failed"));
        failed_state->complete_ = true;
        failed_state->data_signal_.notify();
        co_await failed_tasks.join();
        failed_state->body_operation_scope_.close();
        co_await failed_state->body_operation_scope_.close_and_join();
        RUVIA_CHECK(failure_as_expected);
        RUVIA_CHECK_EQ(std::string_view(failed_state->buffered_), body);
        RUVIA_CHECK_EQ(failed_state->headers_.front().value(), std::string_view(header_value));

        cancelled_state->memory_domain()->detach_transport_bindings(pool);
        RUVIA_CHECK(cancelled_state->pool_ == nullptr);
        RUVIA_CHECK(failed_state->pool_ == nullptr);
        cancelled_state->release_reference();
        RUVIA_CHECK(upstream.large_allocations() > 0);
        failed_state->release_reference();
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.live_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.large_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.large_bytes(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.small_cached_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.small_cached_bytes(), std::size_t{0});
        RUVIA_CHECK(budget_lifetime.expired());

        pool.close_now();
        co_await pool.join();
    });
}

RUVIA_TEST(client_response_memory_budget_is_shared_and_charged_before_buffer_growth) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    allocation_counter upstream;
    auto budget = std::make_shared<ruvia::detail::http_client_result_budget_domain>(
        ruvia::http_client_result_budget_config{.max_in_flight_bytes_ = 2048});
    {
        auto first = ruvia::detail::http_client_response_memory_domain::create(worker.handle_, budget, upstream);
        auto second = ruvia::detail::http_client_response_memory_domain::create(worker.handle_, budget, upstream);
        for (int iteration = 0; iteration < 16; ++iteration) {
            {
                std::pmr::string held(first->resource());
                held.assign(1024, 'a');
                const auto held_charge = budget->in_flight_bytes();
                RUVIA_CHECK(held_charge >= 1025U);
                {
                    std::pmr::string rejected(second->resource());
                    // Debug standard libraries may allocate per-container
                    // metadata from the PMR resource, independently of string
                    // capacity. Preserve that live baseline when testing the
                    // failed growth's transactional budget behavior.
                    const auto pre_growth_charge = budget->in_flight_bytes();
                    RUVIA_CHECK(pre_growth_charge >= held_charge);
                    bool limited = false;
                    try {
                        rejected.assign(1024, 'b');
                    } catch (const ruvia::http_client_error& error) {
                        limited = error.code() == ruvia::http_client_error::code_type::result_budget_exceeded;
                    }
                    RUVIA_CHECK(limited);
                    RUVIA_CHECK_EQ(budget->in_flight_bytes(), pre_growth_charge);
                    RUVIA_CHECK_EQ(held.size(), 1024U);
                }
                RUVIA_CHECK_EQ(budget->in_flight_bytes(), held_charge);
            }
            RUVIA_CHECK_EQ(budget->in_flight_bytes(), 0U);
        }
    }
    RUVIA_CHECK_EQ(upstream.live_bytes(), 0U);
}

RUVIA_TEST(client_response_decoder_allocations_share_the_receive_budget) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    auto budget = std::make_shared<ruvia::detail::http_client_result_budget_domain>(
        ruvia::http_client_result_budget_config{.max_in_flight_bytes_ = 128 * 1024});
    auto domain = ruvia::detail::http_client_response_memory_domain::create(worker.handle_, budget);
    const std::string plain(256 * 1024, 'a');
    constexpr std::array codings{ruvia::http_content_coding::gzip, ruvia::http_content_coding::deflate};
    auto encoded = ruvia::encode_http_content(codings, plain, {.max_encoded_bytes_ = 4096});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    run_operation(worker, io, [&]() -> ruvia::task<void> {
        ruvia::detail::http_client_response_state state_value(worker.handle_, domain->resource());
        state_value.response_body_plan_ = ruvia::plan_http_response_body(ruvia::http_known_method::get, ruvia::http_status::ok);
        state_value.headers_.push_back(
            ruvia::http_header::copy_of("Content-Encoding", "gzip, deflate", state_value.resource_));
        state_value.pending_.assign(encoded.encoded()->bytes());
        ruvia::detail::configure_http_client_response_decoding(state_value);
        bool limited = false;
        try {
            ruvia::detail::decode_http_client_response_content_encoding(state_value, true, plain.size());
        } catch (const ruvia::http_client_error& error) {
            limited = error.code() == ruvia::http_client_error::code_type::result_budget_exceeded;
        }
        RUVIA_CHECK(limited);
        RUVIA_CHECK(state_value.pending_.empty());
        RUVIA_CHECK(state_value.buffered_.empty());
        co_return;
    });
    RUVIA_CHECK_EQ(budget->in_flight_bytes(), 0U);
}
