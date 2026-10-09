#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/error.h"

#include "content_decoding_fixture.h"
#include "context_request_fixture.h"

namespace context_body_decoding_test {

struct context_body_read_observation final {
    std::string body_;
    std::optional<ruvia::http_status_code> error_status_;
};

inline std::unique_ptr<ruvia::scoped_operation<std::string_view>> make_expired_context_text_read() {
    std::unique_ptr<ruvia::scoped_operation<std::string_view>> operation;
    (void)context_request_test::with_context(ruvia::test_request::post("/").body("body"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            operation.reset(new ruvia::scoped_operation<std::string_view>(context_value.req().text()));
            co_return;
        });
    return operation;
}

inline ruvia::task<void> await_expired_context_text_read(
    ruvia::scoped_operation<std::string_view>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline context_body_read_observation read_context_gzip_body(std::string_view encoded) {
    context_body_read_observation observation;
    (void)context_request_test::with_context(
        ruvia::test_request::post("/").header("Content-Encoding", "gzip").body(encoded),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            try {
                observation.body_ = co_await context_value.req().text();
            } catch (const ruvia::http_protocol_error& error) {
                observation.error_status_ = error.status();
            }
            co_return;
        });
    return observation;
}

}  // namespace context_body_decoding_test

using namespace content_decoding_test;       // NOLINT(google-build-using-namespace)
using namespace context_body_decoding_test;  // NOLINT(google-build-using-namespace)
