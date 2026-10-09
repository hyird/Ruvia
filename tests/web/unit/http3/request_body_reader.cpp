#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"

#include "body/http_request_body_facade.h"
#include "http3/http3_request_body_reader.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t returns_{};
    std::size_t body_allocations_{};
    std::size_t body_returns_{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations_;
        if (size >= 512) {
            ++body_allocations_;
        }
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns_;
        if (size >= 512) {
            ++body_returns_;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

ruvia::task<std::optional<std::span<const std::byte>>> read_facade(ruvia::body_reader& reader_value) {
    co_return co_await reader_value.read();
}

ruvia::task<std::optional<std::span<const std::byte>>> read_body(
    ruvia::detail::http3_request_body_reader& reader_value) {
    co_return co_await reader_value.read();
}

template <typename operation_type>
void run_worker_operation(asio::io_context& io, ruvia::event_loop_attachment& attachment, operation_type&& operation) {
    std::exception_ptr failure;
    asio::co_spawn(io, ruvia::as_awaitable(operation()), [&](std::exception_ptr error) {
        failure = error;
        attachment.stop();
    });
    attachment.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

RUVIA_TEST(http3_request_body_reader_resumes_one_producer_without_invalidating_borrowed_chunks) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::detail::http3_request_body_reader body(worker_value, 1024, &resource);
        ruvia::task_scope tasks(worker_value, {.resource_ = &resource});
        ruvia::worker_signal started(worker_value), produced(worker_value), consumed(worker_value);
        const std::string first_bytes(1024, 'a'), second_bytes(1024, 'b');
        bool completed = false;
        bool expired = false;
        {
            auto cold = body.wait_for_space();  // Discarding a lazy wait reserves no producer lane.
        }
        auto producer_value = [&]() -> ruvia::task<void> {
            for (unsigned step = 0; step < 32; ++step) {
                started.notify();
                if (!(co_await body.wait_for_space(1024))) {
                    co_return;
                }
                RUVIA_CHECK(body.available_capacity() == 1024);
                RUVIA_CHECK(body.enqueue(second_bytes));
                produced.notify();
                co_await consumed.wait();
            }
        };
        auto watchdog_value = [&]() -> ruvia::task<void> {
            (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(500));
            if (!completed) {
                expired = true;
                (void)body.shutdown();
                started.notify();
                produced.notify();
                consumed.notify();
            }
        };
        RUVIA_CHECK(body.enqueue(first_bytes));
        tasks.spawn(producer_value());
        tasks.spawn(watchdog_value());
        std::optional<std::size_t> allocation_baseline;
        for (unsigned step = 0; step < 32; ++step) {
            co_await started.wait();
            if (expired) {
                break;
            }
            bool concurrent_rejected = false;
            try {
                (void)co_await body.wait_for_space();
            } catch (const std::logic_error&) {
                concurrent_rejected = true;
            }
            RUVIA_CHECK(concurrent_rejected);
            const auto first = co_await body.read();
            co_await produced.wait();
            if (expired) {
                break;
            }
            RUVIA_CHECK(first && std::string_view(reinterpret_cast<const char*>(first->data()), first->size()) == first_bytes);
            const auto second = co_await body.read();
            RUVIA_CHECK(second && std::string_view(reinterpret_cast<const char*>(second->data()), second->size()) == second_bytes);
            const auto live = resource.allocations_ - resource.returns_;
            if (allocation_baseline) {
                RUVIA_CHECK_EQ(live, *allocation_baseline);
            } else {
                allocation_baseline = live;
            }
            if (step + 1 < 32) {
                RUVIA_CHECK(body.enqueue(first_bytes));
            }
            consumed.notify();
        }
        completed = true;
        RUVIA_CHECK(!expired);
        if (!expired) {
            RUVIA_CHECK(body.finish());
            RUVIA_CHECK(!(co_await body.read()));
        }
        co_await tasks.join();
    };
    run_worker_operation(io, attachment, operation);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_request_body_reader_terminal_states_wake_blocked_producer) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    auto operation = [&]() -> ruvia::task<void> {
        using reader = ruvia::detail::http3_request_body_reader;
        for (const auto terminal : {reader::terminal_type::fin, reader::terminal_type::error,
                 reader::terminal_type::cancelled, reader::terminal_type::shutdown}) {
            reader body(worker_value, 512, &resource);
            ruvia::task_scope tasks(worker_value, {.resource_ = &resource});
            ruvia::worker_signal started(worker_value);
            RUVIA_CHECK(body.enqueue(std::string(512, 'a')));
            const auto borrowed = co_await body.read();
            RUVIA_CHECK(body.enqueue(std::string(512, 'b')));
            bool resumed = false;
            bool available = true;
            auto producer_value = [&]() -> ruvia::task<void> {
                started.notify();
                available = co_await body.wait_for_space();
                resumed = true;
            };
            tasks.spawn(producer_value());
            co_await started.wait();
            RUVIA_CHECK(!resumed);
            switch (terminal) {
                case reader::terminal_type::fin:
                    RUVIA_CHECK(body.finish());
                    break;
                case reader::terminal_type::error:
                    RUVIA_CHECK(body.fail(std::make_error_code(std::errc::protocol_error)));
                    break;
                case reader::terminal_type::cancelled:
                    RUVIA_CHECK(body.cancel());
                    break;
                case reader::terminal_type::shutdown:
                    RUVIA_CHECK(body.shutdown());
                    break;
                case reader::terminal_type::open:
                    break;
            }
            co_await tasks.join();
            RUVIA_CHECK(resumed && !available);
            RUVIA_CHECK(borrowed && borrowed->front() == std::byte{'a'});
            RUVIA_CHECK(!(co_await body.wait_for_space()));
            bool too_large = false;
            try {
                (void)co_await body.wait_for_space(513);
            } catch (const std::invalid_argument&) {
                too_large = true;
            }
            RUVIA_CHECK(too_large);
        }
    };
    run_worker_operation(io, attachment, operation);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_request_body_reader_owns_bounded_chunks_and_orders_fin) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 16});
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    bool accepted = false;
    bool rejected = false;
    bool finished = false;
    bool late_rejected = false;
    bool view_okay = false;
    bool eof = false;
    bool failed = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            ruvia::detail::body_reader_binding<ruvia::detail::http3_request_body_reader> binding(
                worker_value, 1024, &resource);
            auto& body = binding.reader();
            const std::string first_storage(600, 'a');
            const std::string second_storage(700, 'b');
            const std::string rejected_storage(1100, 'x');
            const std::string_view first_bytes(first_storage);
            const std::string_view second_bytes(second_storage);
            accepted = body.enqueue(first_bytes);
            rejected = !body.enqueue(rejected_storage);

            auto first = co_await ruvia::as_awaitable(read_facade(binding.facade()));
            const auto first_view = first.has_value() ? std::string_view(
                reinterpret_cast<const char*>(first->data()), first->size()) : std::string_view{};
            accepted = accepted && body.enqueue(second_bytes);
            view_okay = first_view == first_bytes;
            finished = body.finish();
            auto second = co_await ruvia::as_awaitable(read_body(body));
            late_rejected = !body.enqueue("late");
            const bool second_okay = second.has_value() && second->size() == second_bytes.size() &&
                std::string_view(reinterpret_cast<const char*>(second->data()), second->size()) ==
                    second_bytes;
            view_okay = view_okay && second_okay;
            auto end = co_await ruvia::as_awaitable(read_body(body));
            eof = !end.has_value();
        } catch (...) {
            failed = true;
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(accepted);
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(finished);
    RUVIA_CHECK(late_rejected);
    RUVIA_CHECK(view_okay);
    RUVIA_CHECK(eof);
    RUVIA_CHECK(!failed);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_request_body_reader_repeated_reads_release_prior_chunk_without_losing_next) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    bool correct = true;
    bool failed = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            ruvia::detail::http3_request_body_reader reader_value(worker_value, 1024, &resource);
            for (char digit = '0'; digit != '8'; ++digit) {
                const std::string payload_value(600, digit);
                const bool enqueued = reader_value.enqueue(payload_value);
                correct = correct && enqueued;
                if (!enqueued) {
                    break;
                }
                auto view = co_await ruvia::as_awaitable(read_body(reader_value));
                correct = correct && view.has_value() && view->size() == payload_value.size() &&
                    std::string_view(reinterpret_cast<const char*>(view->data()), view->size()) == payload_value;
                correct = correct && resource.body_allocations_ == resource.body_returns_ + 1;
            }
            const bool finished = reader_value.finish();
            correct = correct && finished;
            auto end = co_await ruvia::as_awaitable(read_body(reader_value));
            correct = correct && !end.has_value();
            correct = correct && resource.body_allocations_ == resource.body_returns_;
        } catch (...) {
            failed = true;
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(correct);
    RUVIA_CHECK(!failed);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_request_body_reader_cancel_wakes_and_joins_a_suspended_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    bool cancelled = false;
    bool joined = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        ruvia::detail::http3_request_body_reader reader_value(worker_value, 64, &resource);
        asio::post(io, [&] { cancelled = reader_value.cancel(); });
        try {
            (void)co_await ruvia::as_awaitable(read_body(reader_value));
        } catch (const std::system_error& error) {
            joined = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(cancelled);
    RUVIA_CHECK(joined);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_request_body_reader_error_discards_unread_bytes_without_invalidating_active_view) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    bool retained = false;
    bool rejected = false;
    bool released = false;
    bool failed = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            ruvia::detail::http3_request_body_reader reader_value(worker_value, 1024, &resource);
            const std::string first(600, 'a');
            const std::string pending(700, 'b');
            (void)reader_value.enqueue(first);
            auto view = co_await ruvia::as_awaitable(read_body(reader_value));
            (void)reader_value.enqueue(pending);
            RUVIA_CHECK(reader_value.fail(std::make_error_code(std::errc::bad_message)));
            retained = view.has_value() && view->size() == first.size() &&
                std::string_view(reinterpret_cast<const char*>(view->data()), view->size()) == first;
            RUVIA_CHECK_EQ(reader_value.queued_bytes(), 0U);
            try {
                (void)co_await ruvia::as_awaitable(read_body(reader_value));
            } catch (const std::system_error& error) {
                rejected = error.code() == std::make_error_code(std::errc::bad_message);
            }
            released = resource.body_allocations_ == resource.body_returns_;
        } catch (...) {
            failed = true;
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(retained);
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(released);
    RUVIA_CHECK(!failed);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_request_body_reader_shutdown_wakes_and_joins_a_suspended_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    bool shutdown = false;
    bool joined = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        ruvia::detail::http3_request_body_reader reader_value(worker_value, 64, &resource);
        asio::post(io, [&] { shutdown = reader_value.shutdown(); });
        try {
            (void)co_await ruvia::as_awaitable(read_body(reader_value));
        } catch (const std::system_error& error) {
            joined = error.code() == std::make_error_code(std::errc::owner_dead);
        }
        io.stop(); }, asio::detached);

    io.run();
    RUVIA_CHECK(shutdown);
    RUVIA_CHECK(joined);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_request_body_reader_cold_task_does_not_allocate_body_storage) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    counting_resource resource;
    {
        ruvia::detail::http3_request_body_reader reader_value(worker_value, 64, &resource);
        auto cold = reader_value.read();
        (void)cold;
    }
    RUVIA_CHECK_EQ(resource.body_allocations_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.body_returns_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}
