#include <exception>
#include <optional>
#include <string>

#include "ruvia/core/event_loop_attachment.h"

#include "http/http_tunnel_session.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct tunnel_transport_state {
    std::string output_;
    unsigned reads_{};
    unsigned finishes_{};
    bool aborted_{};
};
struct tunnel_transport {
    tunnel_transport_state& state_;
    ruvia::task<ruvia::detail::http_stream_read_result> read_more(std::pmr::string& bytes_value) {
        if (state_.aborted_) {
            co_return ruvia::detail::http_stream_read_result::make_failure(std::make_error_code(std::errc::operation_canceled));
        }
        if (++state_.reads_ == 1) {
            bytes_value.append(512, 'r');
            co_return ruvia::detail::http_stream_read_result::make_data();
        }
        co_return ruvia::detail::http_stream_read_result::make_end();
    }
    ruvia::task<std::error_code> write_bytes(std::string_view bytes_value, ruvia::detail::http_stream_end end) {
        if (state_.aborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        state_.output_.append(bytes_value);
        if (end == ruvia::detail::http_stream_end::end) {
            ++state_.finishes_;
        }
        co_return std::error_code{};
    }
    void abort() noexcept {
        state_.aborted_ = true;
    }
};
}  // namespace
RUVIA_TEST(http_tunnel_owns_cold_writes_enforces_direction_lanes_and_preserves_reads_after_finish) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource resource;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            tunnel_transport_state state;
            const auto& worker_value = attachment.loop().handle();
            ruvia::detail::http_tunnel_session session_value(tunnel_transport{state}, worker_value, resource);
            auto& tunnel = session_value.tunnel();
            {
                auto cold = tunnel.write("discarded");
            }
            RUVIA_CHECK(state.output_.empty());
            std::string bytes_value(1024, 'w');
            auto write = tunnel.write(std::string_view(bytes_value));
            bytes_value.assign("mutated");
            bool overlap = false;
            try {
                auto busy = tunnel.finish();
            } catch (const std::logic_error&) {
                overlap = true;
            }
            RUVIA_CHECK(overlap);
            auto read = tunnel.read();
            co_await std::move(write);
            auto retained = co_await std::move(read);
            RUVIA_CHECK(retained && std::string_view(*retained) == std::string(512, 'r'));
            RUVIA_CHECK(state.output_ == std::string(1024, 'w'));
            co_await tunnel.finish();
            co_await tunnel.finish();
            RUVIA_CHECK_EQ(state.finishes_, 1U);
            RUVIA_CHECK(!(co_await tunnel.read()));
            RUVIA_CHECK(std::string_view(*retained) == std::string(512, 'r'));
            bool after_finish = false;
            try {
                auto invalid = tunnel.write("invalid");
            } catch (const std::logic_error&) {
                after_finish = true;
            }
            RUVIA_CHECK(after_finish);
            co_await session_value.join();
            bool expired = false;
            try {
                auto invalid = tunnel.read();
            } catch (const std::logic_error&) {
                expired = true;
            }
            RUVIA_CHECK(expired);
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto task_value = attachment.loop().start(run());
    io.run();
    task_value.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
