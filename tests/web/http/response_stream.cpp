#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace response_stream_test {

struct stream_lane_observation final {
    std::atomic<bool> write_overlap_rejected_{};
    std::atomic<bool> end_overlap_rejected_{};
    std::atomic<bool> completed_{};
};

const auto stream_lane_observed = std::make_shared<stream_lane_observation>();
[[maybe_unused]] const bool stream_lane_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<stream_lane_observation>>([] { return stream_lane_observed; });
    return true;
}();

class stream_lane_routes final : public ruvia::controller<stream_lane_routes> {
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_STREAM("/server-lazy-stream-output", output);
    RUVIA_ROUTES_END

    ruvia::task<void> output(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<stream_lane_observation>>();
        auto& writer = context_value.stream_text();
        std::string payload = "first";
        auto first = writer.write(payload);
        auto last = writer.writeln("second");
        auto end = writer.end();
        payload.assign("changed");
        {
            auto discarded = writer.write("discarded");
            static_cast<void>(discarded);
        }
        ruvia::task_scope writes(context_value.worker());
        const auto send = [&]() -> ruvia::task<void> {
            co_await std::move(first);
        };
        writes.spawn(send());
        try {
            auto overlap = writer.writeln("overlap");
            co_await std::move(overlap);
        } catch (const std::logic_error&) {
            observed.write_overlap_rejected_ = true;
        }
        try {
            auto overlap = writer.end();
            co_await std::move(overlap);
        } catch (const std::logic_error&) {
            observed.end_overlap_rejected_ = true;
        }
        co_await writes.join();
        co_await std::move(last);
        co_await std::move(end);
        observed.completed_ = true;
    }
};

}  // namespace response_stream_test

using response_stream_test::stream_lane_observed;

// Public stream response planning rejects a head that disagrees with its status.
RUVIA_TEST(response_stream_head_rejects_a_mismatched_status_plan) {
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::created);
    auto plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
        ruvia::http_status::accepted, ruvia::http_response_trailer_intent::none);
    bool rejected = false;
    try {
        (void)ruvia::prepare_http_response_stream_head(
            std::move(response), ruvia::http_response_stream_kind::generic, std::move(plan));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(response_stream_cold_outputs_share_a_lane_only_when_started) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    stream_lane_observed->write_overlap_rejected_ = false;
    stream_lane_observed->end_overlap_rejected_ = false;
    stream_lane_observed->completed_ = false;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = server.endpoint().port(),
                                                         .request_timeout_ = std::chrono::seconds(5)});
        try {
            auto response = co_await client.handle().send({.target_ = "/server-lazy-stream-output"});
            RUVIA_CHECK(response.status() == ruvia::http_status::ok);
            auto body = co_await response.body().read_all();
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), "firstsecond\n");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    done.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(stream_lane_observed->write_overlap_rejected_);
    RUVIA_CHECK(stream_lane_observed->end_overlap_rejected_);
    RUVIA_CHECK(stream_lane_observed->completed_);
}
