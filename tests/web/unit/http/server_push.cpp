#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http1_server_request_parser.h"

#include "client/http_client_config_validation.h"
#include "context/context_access.h"
#include "context/http_push_output.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct push_sink final {
    const ruvia::worker_handle& worker_;
    ruvia::worker_signal entered_;
    ruvia::stop_source stop_;
    bool delay_{};
    bool fail_{};
    unsigned writes_{};
    std::string path_;
    std::string header_;
    explicit push_sink(const ruvia::worker_handle& handle)
        : worker_(handle),
          entered_(handle) {}
    static ruvia::task<bool> submit(void* raw, ruvia::http_push_request_view request) {
        auto& sink_value = *static_cast<push_sink*>(raw);
        if (sink_value.delay_) {
            sink_value.entered_.notify();
            const auto result_value = co_await ruvia::sleep_for(sink_value.worker_, std::chrono::seconds(10), sink_value.stop_.token());
            if (result_value == ruvia::timer_sleep_result::stop_requested) {
                throw std::runtime_error("cancelled");
            }
        }
        if (sink_value.fail_) {
            throw std::runtime_error("push failed");
        }
        sink_value.path_ = request.path_;
        sink_value.header_ = request.headers_.front().value();
        ++sink_value.writes_;
        co_return true;
    }
};
}  // namespace

RUVIA_TEST(context_push_owns_inputs_reclaims_each_operation_and_handles_cold_failure_cancel_expiry) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        ruvia::test::counting_memory_resource memory;
        ruvia::worker_memory worker_memory;
        ruvia::request_memory request_memory(worker_memory);
        const auto parsed_value = ruvia::http1_server_request_parser{}.parse_message("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
        const ruvia::stop_token stop;
        push_sink sink(worker_value);
        auto make_context = [&](auto& output) {
            return ruvia::detail::context_access::make(request_memory, parsed_value.request_,
                ruvia::detail::context_services(worker_value, stop).with_push_output(output));
        };
        {
            ruvia::detail::http_push_output output(&memory, &sink, &push_sink::submit);
            auto context_value = make_context(output);
            std::string path = "/" + std::string(80, 'a');
            std::string value(80, 'h');
            const std::array<ruvia::http_header_view, 1> fields_value{{{"x-push", value}}};
            const ruvia::http_push_request_view request{.authority_ = "example.test", .path_ = path, .headers_ = fields_value};
            {
                auto cold = context_value.push(request);
                RUVIA_CHECK(memory.live_allocations() > 0);
            }
            RUVIA_CHECK_EQ(sink.writes_, 0U);
            RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
            for (unsigned repeat = 0; repeat != 128; ++repeat) {
                path.assign("/" + std::string(80, 'a'));
                value.assign(80, 'h');
                const std::array<ruvia::http_header_view, 1> inputs{{{"x-push", value}}};
                auto operation = context_value.push({.authority_ = "example.test", .path_ = path, .headers_ = inputs});
                bool busy = false;
                try {
                    auto overlap = context_value.push(request);
                } catch (const std::logic_error&) {
                    busy = true;
                }
                RUVIA_CHECK(busy);
                path.assign("changed");
                value.assign("changed");
                RUVIA_CHECK(co_await std::move(operation));
                RUVIA_CHECK(sink.path_ == "/" + std::string(80, 'a') && sink.header_ == std::string(80, 'h'));
                RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
            }
            const std::array<ruvia::http_header_view, 1> valid_fields{{{"x-push", "failure"}}};
            sink.fail_ = true;
            bool failed = false;
            try {
                (void)co_await context_value.push({.authority_ = "example.test", .headers_ = valid_fields});
            } catch (const std::runtime_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
            sink.fail_ = false;
            sink.delay_ = true;
            ruvia::task_scope tasks(worker_value);
            bool cancelled = false;
            auto wait = [&]() -> ruvia::task<void> {
                try {
                    (void)co_await context_value.push({.authority_ = "example.test", .headers_ = valid_fields});
                } catch (const std::runtime_error&) {
                    cancelled = true;
                }
            };
            tasks.spawn(wait());
            co_await sink.entered_.wait();
            sink.stop_.request_stop();
            co_await tasks.join();
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        }
        {
            std::optional<ruvia::detail::http_push_output> output;
            output.emplace(&memory, &sink, &push_sink::submit);
            auto context_value = make_context(*output);
            auto expired = context_value.push({.authority_ = "example.test"});
            output.reset();
            bool rejected = false;
            try {
                (void)co_await std::move(expired);
            } catch (const std::logic_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        }
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_push_configuration_rejects_zero_bounds_timeout_and_unverified_https) {
    ruvia::http_client_config config{.host_ = "example.test"};
    for (unsigned mode = 0; mode != 4; ++mode) {
        config.push_ = {.enabled_ = true};
        config.tls_peer_verification_ = ruvia::tls_peer_verification_policy::verify;
        if (mode == 0) {
            config.push_.max_queued_pushes_ = 0;
        }
        if (mode == 1) {
            config.push_.max_concurrent_pushes_ = 0;
        }
        if (mode == 2) {
            config.push_.timeout_ = std::chrono::milliseconds::zero();
        }
        if (mode == 3) {
            config.tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification;
        }
        bool rejected = false;
        try {
            ruvia::detail::validate_http_client_config(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
}
