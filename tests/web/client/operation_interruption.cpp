#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "controller.h"
#include "event_loop.h"
#include "http_client.h"
#include "http_server_fixture.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "task_scope.h"
#include "worker_task_fixture.h"

namespace client_operation_interruption_test {

void exercise(const std::string& url, ruvia::http_client_versions versions) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client owner(attachment.loop(), {.base_url_ = url, .versions_ = versions});
        try {
            co_await ruvia::test::client_operations::exercise(owner.client(), worker);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await owner.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace client_operation_interruption_test

TEST(Ruvia, http_client_operations_end_one_wait_or_abort_their_stream_over_http1_and_http2) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const std::string url("http://127.0.0.1:" + std::string(std::to_string(server.endpoint().port())));
    std::vector<ruvia::http_client_versions> protocols({ruvia::http_client_versions{.http2_ = false}});
#ifdef RUVIA_ENABLE_HTTP2
    protocols.push_back(ruvia::http_client_versions{.http1_ = false});
#endif
    for (const auto versions : protocols) {
        SCOPED_TRACE(versions.http1_ ? "HTTP/1.1" : "HTTP/2");
        client_operation_interruption_test::exercise(url, versions);
        server.finish();
    }
}
