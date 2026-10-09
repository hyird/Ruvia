#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http2_framing.h"

#include "context/context_access.h"
#include "context/http_connection_advertisement_output.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using output_type = ruvia::detail::http_connection_advertisement_output;
struct sink final {
    const ruvia::worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    ruvia::worker_signal entered_;
    ruvia::stop_source stop_;
    std::string wire_;
    bool fail_{};
    bool delay_{};
    sink(const ruvia::worker_handle& worker_value, std::pmr::memory_resource* resource)
        : worker_(worker_value),
          resource_(resource),
          entered_(worker_value) {}
    static ruvia::task<void> origins(void* raw, std::span<const std::string_view> origins) {
        auto& sink_value = *static_cast<sink*>(raw);
        if (sink_value.delay_) {
            sink_value.entered_.notify();
            const auto result_value = co_await ruvia::sleep_for(sink_value.worker_, std::chrono::seconds(10), sink_value.stop_.token());
            if (result_value == ruvia::timer_sleep_result::stop_requested) {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
        }
        if (sink_value.fail_) {
            throw std::system_error(std::make_error_code(std::errc::broken_pipe));
        }
        const auto encoded = ruvia::encode_http2_origin_frame(origins, 16384, sink_value.resource_);
        if ((encoded.index() != 0)) {
            throw std::invalid_argument("invalid advertised origin");
        }
        sink_value.wire_.append(std::get<0>(encoded).data(), std::get<0>(encoded).size());
    }
    static ruvia::task<void> service(void* raw, std::string_view value) {
        auto& sink_value = *static_cast<sink*>(raw);
        const auto encoded = ruvia::encode_http2_alternative_service_frame(1, {}, value, 16384, sink_value.resource_);
        if ((encoded.index() != 0)) {
            throw std::invalid_argument("invalid alternative service");
        }
        sink_value.wire_.append(std::get<0>(encoded).data(), std::get<0>(encoded).size());
        co_return;
    }
};
}  // namespace

RUVIA_TEST(context_connection_advertisements_own_inputs_reclaim_storage_and_handle_cancel_failure_and_expiry) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto run = [&]() -> ruvia::task<void> {
        ruvia::test::counting_memory_resource memory;
        ruvia::worker_memory worker_memory;
        ruvia::request_memory request_memory(worker_memory);
        const auto parsed_value = ruvia::http1_server_request_parser{}.parse_message("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
        ruvia::stop_source worker_stop;
        const auto token = worker_stop.token();
        sink sink_value(worker_value, &memory);
        output_type output(&memory, &sink_value, sink::origins, sink::service);
        auto context_value = ruvia::detail::context_access::make(request_memory, parsed_value.request_,
            ruvia::detail::context_services(worker_value, token).with_tls_transport("127.0.0.1", {}).with_connection_advertisements(output));
        std::string value = "https://" + std::string(60, 'a') + ".example.test";
        const std::array<std::string_view, 1> origins{value};
        {
            auto cold = context_value.advertise_origins(origins);
            RUVIA_CHECK(memory.live_allocations() > 0);
        }
        RUVIA_CHECK(sink_value.wire_.empty());
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        {
            auto operation = context_value.advertise_origins(origins);
            value.assign("changed");
            bool busy = false;
            try {
                auto overlap = context_value.advertise_alternative_service("clear");
            } catch (const std::logic_error&) {
                busy = true;
            }
            RUVIA_CHECK(busy);
            co_await std::move(operation);
        }
        const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(sink_value.wire_));
        RUVIA_CHECK(frame.has_value());
        const auto decoded = ruvia::decode_http_origin_advertisement(std::span<const char>(sink_value.wire_).subspan(9, frame->length_));
        const auto expected = "https://" + std::string(60, 'a') + ".example.test";
        RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).origins_.size() == 1 && std::string_view(std::get<0>(decoded).origins_[0]) == expected);
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        for (unsigned repeat = 0; repeat != 128; ++repeat) {
            std::string service = "h3=\":443\"; ma=86400";
            auto operation = context_value.advertise_alternative_service(service);
            service.assign("changed");
            co_await std::move(operation);
            RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        }
        const std::array<std::string_view, 1> valid{"https://example.test"};
        sink_value.fail_ = true;
        bool failed = false;
        try {
            co_await context_value.advertise_origins(valid);
        } catch (const std::system_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        sink_value.fail_ = false;
        sink_value.delay_ = true;
        bool cancelled = false;
        {
            ruvia::task_scope tasks(worker_value);
            auto cancelling = [&]() -> ruvia::task<void> {
                try {
                    co_await context_value.advertise_origins(valid);
                } catch (const std::system_error& error) {
                    cancelled = error.code() == std::errc::operation_canceled;
                }
            };
            tasks.spawn(cancelling());
            co_await sink_value.entered_.wait();
            sink_value.stop_.request_stop();
            co_await tasks.join();
        }
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        {
            std::optional<output_type> expiring;
            expiring.emplace(&memory, &sink_value, sink::origins, nullptr);
            auto stale = expiring->advertise_origins(valid);
            expiring.reset();
            bool expired = false;
            try {
                co_await std::move(stale);
            } catch (const std::logic_error&) {
                expired = true;
            }
            RUVIA_CHECK(expired);
        }
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}
