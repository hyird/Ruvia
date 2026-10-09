#include <algorithm>
#include <array>
#include <future>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http1_server_request_parser.h"

#include "context/context_access.h"
#include "memory_resource_fixture.h"
#include "server/http1_interim_response_sink.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct output_stream final {
    asio::io_context& io_;
    std::string wire_;
    bool fail_{};
    using executor_type = asio::io_context::executor_type;
    executor_type get_executor() noexcept {
        return io_.get_executor();
    }
    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type handler) {
        const auto count = std::min(asio::buffer_size(buffers), std::size_t{7});
        std::array<char, 7> bytes_value{};
        asio::buffer_copy(asio::buffer(bytes_value), buffers, count);
        if (!fail_) {
            wire_.append(bytes_value.data(), count);
        }
        asio::post(io_, [handler = std::move(handler), count, fail = fail_]() mutable {
            handler(fail ? std::make_error_code(std::errc::broken_pipe) : std::error_code{}, fail ? 0 : count);
        });
    }
};
}  // namespace

RUVIA_TEST(http1_context_interim_output_owns_fields_drops_cold_operations_and_rejects_final_overlap) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource upstream;
    {
        ruvia::worker_memory memory(upstream);
        ruvia::request_memory request_memory(memory);
        const auto worker_value = attachment.loop().handle();
        ruvia::stop_source stop;
        const auto stop_token_value = stop.token();
        output_stream stream{io};
        ruvia::detail::http1_interim_response_sink sink_value(stream, memory.resource());
        const auto parsed_value = ruvia::http1_server_request_parser{}.parse_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        auto context_value = ruvia::detail::context_access::make(request_memory, parsed_value.request_,
            ruvia::detail::context_services(worker_value, stop_token_value).with_interim_output(sink_value.output()));
        auto exercise = [&]() -> ruvia::task<void> {
            std::string field(512, 'h');
            const std::array headers{ruvia::http_header_view("Link", field)};
            {
                auto cold = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, headers));
            }
            RUVIA_CHECK(stream.wire_.empty());
            auto operation = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, headers));
            field.assign("changed");
            bool overlap = false;
            try {
                sink_value.output().commit_final();
            } catch (const std::logic_error&) {
                overlap = true;
            }
            RUVIA_CHECK(overlap);
            co_await std::move(operation);
            RUVIA_CHECK_EQ(stream.wire_, "HTTP/1.1 103 Early Hints\r\nLink: " + std::string(512, 'h') + "\r\n\r\n");
            const auto before = stream.wire_;
            const std::array invalid{ruvia::http_header_view("Content-Length", "0")};
            bool rejected = false;
            try {
                co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::continue_value, invalid));
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(stream.wire_, before);
            stream.fail_ = true;
            bool failed = false;
            try {
                co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::continue_value));
            } catch (const std::system_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            sink_value.output().commit_final();
            rejected = false;
            try {
                auto invalid_after_final = context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
            } catch (const std::logic_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        };
        auto run = [&]() -> ruvia::task<void> {
            try {
                co_await exercise();
            } catch (...) {
                attachment.stop();
                throw;
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        root.get();
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
}
